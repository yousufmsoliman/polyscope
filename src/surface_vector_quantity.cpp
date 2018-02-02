#include "polyscope/surface_vector_quantity.h"

#include "polyscope/gl/shaders.h"
#include "polyscope/gl/shaders/vector_shaders.h"
#include "polyscope/polyscope.h"
#include "polyscope/trace_vector_field.h"

#include "imgui.h"

#include "Eigen/Dense"

namespace polyscope {

SurfaceVectorQuantity::SurfaceVectorQuantity(std::string name, SurfaceMesh* mesh_, MeshElement definedOn_,
                                             VectorType vectorType_)

    : SurfaceQuantity(name, mesh_), vectorType(vectorType_), definedOn(definedOn_) {

  // Don't forget to call finishConstructing() in children classes!
}

SurfaceVectorQuantity::~SurfaceVectorQuantity() {
  safeDelete(program);
  safeDelete(ribbonArtist);
}

void SurfaceVectorQuantity::finishConstructing() {

  // Create a mapper (default mapper is identity)
  if (vectorType == VectorType::AMBIENT) {
    mapper.setMinMax(vectors);
  } else {
    mapper = AffineRemapper<Vector3>(vectors, DataType::MAGNITUDE);
  }

  // Default viz settings
  if (vectorType != VectorType::AMBIENT) {
    lengthMult = .02;
  } else {
    lengthMult = 1.0;
  }
  radiusMult = .0005;
  vectorColor = parent->colorManager.getNextSubColor(name);
}

void SurfaceVectorQuantity::draw() {
  if (!enabled || ribbonEnabled) return;

  if (program == nullptr) prepare();

  // Set uniforms
  glm::mat4 viewMat = view::getCameraViewMatrix();
  program->setUniform("u_viewMatrix", glm::value_ptr(viewMat));

  glm::mat4 projMat = view::getCameraPerspectiveMatrix();
  program->setUniform("u_projMatrix", glm::value_ptr(projMat));

  Vector3 eyePos = view::getCameraWorldPosition();
  program->setUniform("u_eye", eyePos);

  program->setUniform("u_lightCenter", state::center);
  program->setUniform("u_lightDist", 5 * state::lengthScale);
  program->setUniform("u_radius", radiusMult * state::lengthScale);
  program->setUniform("u_color", vectorColor);

  if (vectorType == VectorType::AMBIENT) {
    program->setUniform("u_lengthMult", 1.0);
  } else {
    program->setUniform("u_lengthMult", lengthMult * state::lengthScale);
  }

  program->draw();
}

void SurfaceVectorQuantity::prepare() {
  program = new gl::GLProgram(&PASSTHRU_VECTOR_VERT_SHADER, &VECTOR_GEOM_SHADER, &SHINY_VECTOR_FRAG_SHADER,
                              gl::DrawMode::Points);

  // Fill buffers
  std::vector<Vector3> mappedVectors;
  for (Vector3 v : vectors) {
    mappedVectors.push_back(mapper.map(v));
  }

  program->setAttribute("a_vector", mappedVectors);
  program->setAttribute("a_position", vectorRoots);
}

void SurfaceVectorQuantity::drawUI() {


  if (ImGui::TreeNode((name + " (" + getMeshElementTypeName(definedOn) + " vector)").c_str())) {
    ImGui::Checkbox("Enabled", &enabled);
    ImGui::SameLine();
    ImGui::ColorEdit3("Color", (float*)&vectorColor, ImGuiColorEditFlags_NoInputs);

    // Only get to set length for non-ambient vectors
    if (vectorType != VectorType::AMBIENT) {
      ImGui::SliderFloat("Length", &lengthMult, 0.0, .1, "%.5f", 3.);
    }

    ImGui::SliderFloat("Radius", &radiusMult, 0.0, .1, "%.5f", 3.);

    { // Draw max and min magnitude
      ImGui::TextUnformatted(mapper.printBounds().c_str());
    }

    drawSubUI();

    ImGui::TreePop();
  }
}

void SurfaceVectorQuantity::drawSubUI() {}


// ========================================================
// ==========           Vertex Vector            ==========
// ========================================================

SurfaceVertexVectorQuantity::SurfaceVertexVectorQuantity(std::string name, VertexData<Vector3>& vectors_,
                                                         SurfaceMesh* mesh_, VectorType vectorType_)

    : SurfaceVectorQuantity(name, mesh_, MeshElement::VERTEX, vectorType_) {

  vectorField = parent->transfer.transfer(vectors_);
  for (VertexPtr v : parent->mesh->vertices()) {
    vectorRoots.push_back(parent->geometry->position(v));
    vectors.push_back(vectorField[v]);
  }

  finishConstructing();
}

void SurfaceVertexVectorQuantity::buildInfoGUI(VertexPtr v) {
  ImGui::TextUnformatted(name.c_str());
  ImGui::NextColumn();

  std::stringstream buffer;
  buffer << vectorField[v];
  ImGui::TextUnformatted(buffer.str().c_str());

  ImGui::NextColumn();
  ImGui::NextColumn();
  ImGui::Text("magnitude: %g", norm(vectorField[v]));
  ImGui::NextColumn();
}


// ========================================================
// ==========            Face Vector             ==========
// ========================================================

SurfaceFaceVectorQuantity::SurfaceFaceVectorQuantity(std::string name, FaceData<Vector3>& vectors_, SurfaceMesh* mesh_,
                                                     VectorType vectorType_)
    : SurfaceVectorQuantity(name, mesh_, MeshElement::FACE, vectorType_) {

  // Copy the vectors
  vectorField = parent->transfer.transfer(vectors_);
  for (FacePtr f : parent->mesh->faces()) {
    vectorRoots.push_back(parent->geometry->barycenter(f));
    vectors.push_back(vectorField[f]);
  }

  finishConstructing();
}

void SurfaceFaceVectorQuantity::buildInfoGUI(FacePtr f) {
  ImGui::TextUnformatted(name.c_str());
  ImGui::NextColumn();

  std::stringstream buffer;
  buffer << vectorField[f];
  ImGui::TextUnformatted(buffer.str().c_str());

  ImGui::NextColumn();
  ImGui::NextColumn();
  ImGui::Text("magnitude: %g", norm(vectorField[f]));
  ImGui::NextColumn();
}

// ========================================================
// ==========        Intrinsic Face Vector       ==========
// ========================================================


SurfaceFaceIntrinsicVectorQuantity::SurfaceFaceIntrinsicVectorQuantity(std::string name, FaceData<Complex>& vectors_,
                                                                       SurfaceMesh* mesh_, int nSym_,
                                                                       VectorType vectorType_)
    : SurfaceVectorQuantity(name, mesh_, MeshElement::FACE, vectorType_), nSym(nSym_) {

  GeometryCache<Euclidean>& gc = parent->geometry->cache;
  gc.requireFaceBases();

  double rotAngle = 2.0 * PI / nSym;
  Complex rot = std::exp(IM_I * rotAngle);

  // Copy the vectors
  vectorField = parent->transfer.transfer(vectors_);
  for (FacePtr f : parent->mesh->faces()) {

    Complex angle = std::pow(vectorField[f], 1.0 / nSym);

    for (int iRot = 0; iRot < nSym; iRot++) {
      vectorRoots.push_back(parent->geometry->barycenter(f));

      Vector3 v = gc.faceBases[f][0] * angle.real() + gc.faceBases[f][1] * angle.imag();
      vectors.push_back(v);

      angle *= rot;
    }
  }

  finishConstructing();
}

void SurfaceFaceIntrinsicVectorQuantity::buildInfoGUI(FacePtr f) {
  ImGui::TextUnformatted(name.c_str());
  ImGui::NextColumn();

  std::stringstream buffer;
  buffer << vectorField[f];
  ImGui::TextUnformatted(buffer.str().c_str());

  ImGui::NextColumn();
  ImGui::NextColumn();
  ImGui::Text("magnitude: %g", norm(vectorField[f]));
  ImGui::NextColumn();
}

void SurfaceFaceIntrinsicVectorQuantity::draw() {
  SurfaceVectorQuantity::draw();

  if (ribbonEnabled) {

    // Make sure we have a ribbon artist
    if (ribbonArtist == nullptr) {

      // Warning: expensive... Creates noticeable UI lag
      ribbonArtist = new RibbonArtist(traceField(parent->geometry, vectorField, nSym, 2500));
    }


    if (enabled) {
      ribbonArtist->draw();
    }
  }
}

void SurfaceFaceIntrinsicVectorQuantity::drawSubUI() {

  ImGui::Checkbox("Draw ribbon", &ribbonEnabled);
  if (ribbonEnabled && ribbonArtist != nullptr) {
    ribbonArtist->buildParametersGUI();
  }
}

// ========================================================
// ==========        Intrinsic One Form          ==========
// ========================================================


SurfaceOneFormIntrinsicVectorQuantity::SurfaceOneFormIntrinsicVectorQuantity(
    std::string name, EdgeData<double>& oneForm_, SurfaceMesh* mesh_, VectorType vectorType_)
    : SurfaceVectorQuantity(name, mesh_, MeshElement::FACE, vectorType_) {

  GeometryCache<Euclidean>& gc = parent->geometry->cache;
  gc.requireFaceBases();
  gc.requireHalfedgeVectors();

  // Copy the vectors
  oneForm = parent->transfer.transfer(oneForm_);
  mappedVectorField = FaceData<Complex>(parent->mesh);
  for (FacePtr f : parent->mesh->faces()) {

    // Find the best-approximating vector field in each face
    Eigen::Matrix<double, 3, 2> vectorMat;
    Eigen::Vector3d rhsVec;
    unsigned int i = 0;
    for (HalfedgePtr he : f.adjacentHalfedges()) {
      double signVal = (he == he.edge().halfedge()) ? 1.0 : -1.0;
      vectorMat(i,0) = dot(gc.halfedgeVectors[he], gc.faceBases[f][0]);
      vectorMat(i,1) = dot(gc.halfedgeVectors[he], gc.faceBases[f][1]);
      rhsVec[i] = oneForm[he.edge()] * signVal;
      i++;
    }
    auto solver = vectorMat.colPivHouseholderQr();
    Eigen::Vector2d x = solver.solve(rhsVec);
    Complex approxVec{x[0], x[1]};

    mappedVectorField[f] = approxVec;


    // Fill out data for the little arrows
    vectorRoots.push_back(parent->geometry->barycenter(f));
    Vector3 v = gc.faceBases[f][0] * approxVec.real() + gc.faceBases[f][1] * approxVec.imag();
    vectors.push_back(v);
  }

  finishConstructing();
}

void SurfaceOneFormIntrinsicVectorQuantity::buildInfoGUI(EdgePtr e) {
  ImGui::TextUnformatted(name.c_str());
  ImGui::NextColumn();

  ImGui::Text("%g", oneForm[e]);

  ImGui::NextColumn();
}

void SurfaceOneFormIntrinsicVectorQuantity::draw() {
  SurfaceVectorQuantity::draw();

  if (ribbonEnabled) {

    // Make sure we have a ribbon artist
    if (ribbonArtist == nullptr) {

      // Warning: expensive... Creates noticeable UI lag
      ribbonArtist = new RibbonArtist(traceField(parent->geometry, mappedVectorField, 1, 2500));
    }


    if (enabled) {
      ribbonArtist->draw();
    }
  }
}

void SurfaceOneFormIntrinsicVectorQuantity::drawSubUI() {

  ImGui::Checkbox("Draw ribbon", &ribbonEnabled);
  if (ribbonEnabled && ribbonArtist != nullptr) {
    ribbonArtist->buildParametersGUI();
  }
}

} // namespace polyscope
