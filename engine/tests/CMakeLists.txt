add_executable(dt_physics_tests physics_tests.cpp)
# DonTopoEngine expone PhysX y DT_PHYSX_ENABLED como PUBLIC: el test los hereda.
target_link_libraries(dt_physics_tests PRIVATE DonTopoEngine)
target_compile_features(dt_physics_tests PRIVATE cxx_std_20)
