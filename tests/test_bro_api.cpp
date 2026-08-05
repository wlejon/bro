#include <bro/bro_api.hpp>
#include <iostream>
#include <memory>
#include <cassert>

int main() {
    std::cout << "Starting bro_core Native API Smoke Test..." << std::endl;

    // 1. Instantiate bro::Scene
    auto scene = std::make_shared<bro::Scene>("TestScene");
    scene->setBackground(0.1f, 0.2f, 0.3f);

    // 2. Instantiate bro::PerspectiveCamera
    auto camera = std::make_shared<bro::PerspectiveCamera>(60.0f, 1.33f, 0.1f, 1000.0f, "TestCamera");
    camera->setPosition(0.0f, 2.0f, 5.0f);
    camera->lookAt(0.0f, 0.0f, 0.0f);

    // 3. Instantiate bro::WebGLRenderer
    auto renderer = std::make_shared<bro::WebGLRenderer>(800, 600);
    renderer->setClearColor(bro::Color(0.1f, 0.1f, 0.1f, 1.0f));

    // 4. Instantiate bro::PhysicsWorld
    auto physicsWorld = std::make_shared<bro::PhysicsWorld>();
    physicsWorld->setGravity(bro::Vector3(0.0f, -9.81f, 0.0f));

    // Add objects to scene
    auto boxGeo = bro::Geometry::createBox(1.0f, 1.0f, 1.0f);
    auto mat = std::make_shared<bro::Material>();
    mat->setColor(bro::Color(1.0f, 0.0f, 0.0f, 1.0f));
    auto mesh = std::make_shared<bro::Mesh>(boxGeo, mat, "TestBox");
    scene->add(mesh);

    auto ambientLight = std::make_shared<bro::AmbientLight>(bro::Color(1.0f, 1.0f, 1.0f), 0.8f, "TestAmbientLight");
    scene->add(ambientLight);

    auto dirLight = std::make_shared<bro::DirectionalLight>(bro::Color(1.0f, 1.0f, 1.0f), 1.0f, "TestDirLight");
    dirLight->setPosition(5.0f, 10.0f, 5.0f);
    scene->add(dirLight);

    // Add rigid body to physics world
    auto body = std::make_shared<bro::RigidBody>(bro::RigidBodyType::Dynamic, 1.0f);
    body->setPosition(bro::Vector3(0.0f, 10.0f, 0.0f));
    physicsWorld->addBody(body);

    // Tick physics world (physicsWorld->step(0.016))
    physicsWorld->step(0.016);

    bro::Vector3 newPos = body->position();
    std::cout << "Physics body position after step: (" << newPos.x << ", " << newPos.y << ", " << newPos.z << ")" << std::endl;

    // Set render callback
    bool renderCallbackExecuted = false;
    bro::set_render_callback([&renderCallbackExecuted](double dt) {
        renderCallbackExecuted = true;
    });

    bro::tick_render_callback(0.016);
    if (!renderCallbackExecuted) {
        std::cerr << "Error: Render callback was not executed!" << std::endl;
        return 1;
    }

    // Call renderer->render(scene, camera)
    renderer->render(scene, camera);

    // Print exact required success message
    std::cout << "bro_core Native API Smoke Test Passed Successfully!" << std::endl;

    return 0;
}
