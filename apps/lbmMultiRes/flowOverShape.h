#pragma once
#include "Neon/Neon.h"
#include "Neon/domain/mGrid.h"
#include "lbmMultiRes.h"

#include "init.h"


#include "igl/max.h"
#include "igl/min.h"
#include "igl/read_triangle_mesh.h"
#include "igl/remove_unreferenced.h"
#include "igl/writeOBJ.h"

#include "igl/AABB.h"

template <typename T, int Q, typename sdfT>
void initFlowOverShape(Neon::domain::mGrid&                  grid,
                       Neon::domain::mGrid::Field<float>&    sumStore,
                       Neon::domain::mGrid::Field<T>&        fin,
                       Neon::domain::mGrid::Field<T>&        fout,
                       Neon::domain::mGrid::Field<CellType>& cellType,
                       const Neon::double_3d                 inletVelocity,
                       const sdfT                            shapeSDF)
{

    const Neon::index_3d gridDim = grid.getDimension();

    //init fields
    for (int level = 0; level < grid.getDescriptor().getDepth(); ++level) {

        auto container =
            grid.newContainer(
                "Init_" + std::to_string(level), level,
                [&fin, &fout, &cellType, &sumStore, level, gridDim, inletVelocity, shapeSDF](Neon::set::Loader& loader) {
                    auto&   in = fin.load(loader, level, Neon::MultiResCompute::MAP);
                    auto&   out = fout.load(loader, level, Neon::MultiResCompute::MAP);
                    auto&   type = cellType.load(loader, level, Neon::MultiResCompute::MAP);
                    auto&   ss = sumStore.load(loader, level, Neon::MultiResCompute::MAP);
                    const T usqr = (3.0 / 2.0) * (inletVelocity.x * inletVelocity.x + inletVelocity.y * inletVelocity.y + inletVelocity.z * inletVelocity.z);


                    Neon::domain::mGrid::Partition<int8_t> sdf;
                    if constexpr (std::is_same_v<sdfT, Neon::domain::mGrid::Field<int8_t>>) {
                        sdf = shapeSDF.load(loader, level, Neon::MultiResCompute::MAP);
                    }

                    return [=] NEON_CUDA_HOST_DEVICE(const typename Neon::domain::mGrid::Idx& cell) mutable {
                        //velocity and density
                        (void)sdf;
                        (void)shapeSDF;

                        type(cell, 0) = CellType::bulk;

                        for (int q = 0; q < Q; ++q) {
                            ss(cell, q) = 0;
                            in(cell, q) = 0;
                            out(cell, q) = 0;
                        }

                        if (!in.hasChildren(cell)) {
                            const Neon::index_3d idx = in.getGlobalIndex(cell);

                            if (idx.x == 0) {
                                type(cell, 0) = CellType::inlet;
                            }

                            if constexpr (std::is_same_v<sdfT, Neon::domain::mGrid::Field<int8_t>>) {
                                if (sdf(cell) == 1) {
                                    type(cell) = CellType::bounceBack;
                                }
                            } else {
                                if (shapeSDF(idx)) {
                                    type(cell) = CellType::bounceBack;
                                }
                            }

                            //the cell classification
                            if (idx.y == 0 || idx.y == gridDim.y - (1 << level) ||
                                idx.z == 0 || idx.z == gridDim.z - (1 << level)) {
                                type(cell, 0) = CellType::bounceBack;
                            }

                            //population init value
                            for (int q = 0; q < Q; ++q) {
                                T pop_init_val = latticeWeights[q];

                                //bounce back
                                if (type(cell, 0) == CellType::bounceBack) {
                                    pop_init_val = 0;
                                }

                                if (type(cell, 0) == CellType::inlet) {
                                    pop_init_val = 0;

                                    for (int d = 0; d < 3; ++d) {
                                        pop_init_val += latticeVelocity[q][d] * inletVelocity.v[d];
                                    }
                                    pop_init_val *= -6. * latticeWeights[q];
                                }

                                out(cell, q) = pop_init_val;
                                in(cell, q) = pop_init_val;
                            }
                        }
                    };
                });

        container.run(0);
    }


    //init sumStore
    initSumStore<T, Q>(grid, sumStore);
}

template <typename T, int Q>
void flowOverSphere(const Neon::Backend backend,
                    const Params&       params)
{
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);


    Neon::index_3d gridDim(136 * params.scale, 96 * params.scale, 136 * params.scale);

    Neon::index_4d sphere(52 * params.scale, 52 * params.scale, 68 * params.scale, 8 * params.scale);

    int depth = 3;

    const Neon::mGridDescriptor<1> descriptor(depth);

    Neon::domain::mGrid grid(
        backend, gridDim,
        {[&](const Neon::index_3d idx) -> bool {
             return idx.x >= 40 * params.scale && idx.x < 96 * params.scale && idx.y >= 40 * params.scale && idx.y < 64 * params.scale && idx.z >= 40 * params.scale && idx.z < 96 * params.scale;
         },
         [&](const Neon::index_3d idx) -> bool {
             return idx.x >= 24 * params.scale && idx.x < 112 * params.scale && idx.y >= 24 * params.scale && idx.y < 72 * params.scale && idx.z >= 24 * params.scale && idx.z < 112 * params.scale;
         },
         [&](const Neon::index_3d idx) -> bool {
             return true;
         }},
        Neon::domain::Stencil::s19_t(false), descriptor);


    //LBM problem
    const T uin = 0.04;
    //const T               clength = T(grid.getDimension(descriptor.getDepth() - 1).x);
    const T               clength = T(sphere.w / (1 << (depth - 1)));
    const T               visclb = uin * clength / static_cast<T>(params.Re);
    const T               omega = 1.0 / (3. * visclb + 0.5);
    const Neon::double_3d inletVelocity(uin, 0., 0.);

    //auto test = grid.newField<T>("test", 1, 0);
    //test.ioToVtk("Test", true, true, true, false);
    //exit(0);

    //allocate fields
    auto fin = grid.newField<T>("fin", Q, 0);
    auto fout = grid.newField<T>("fout", Q, 0);
    auto storeSum = grid.newField<float>("storeSum", Q, 0);
    auto cellType = grid.newField<CellType>("CellType", 1, CellType::bulk);

    //init fields
    initFlowOverShape<T, Q>(grid, storeSum, fin, fout, cellType, inletVelocity, [sphere] NEON_CUDA_HOST_DEVICE(const Neon::index_3d idx) {
        const T dx = sphere.x - idx.x;
        const T dy = sphere.y - idx.y;
        const T dz = sphere.z - idx.z;

        if ((dx * dx + dy * dy + dz * dz) < sphere.w * sphere.w) {
            return true;
        } else {
            return false;
        }
    });

    //cellType.updateHostData();
    //cellType.ioToVtk("cellType", true, true, true, true);

    runNonUniformLBM<T, Q>(grid,
                           params,
                           clength,
                           omega,
                           visclb,
                           inletVelocity,
                           cellType,
                           storeSum,
                           fin,
                           fout);
}


template <typename T, int Q>
void flowOverMesh(const Neon::Backend backend,
                  const Params&       params)
{
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);

    //define the gird at the finest resolution
    Neon::index_3d gridDim(19 * params.scale, 10 * params.scale, 10 * params.scale);

    //define the box that will encompass the mesh using the index space and dimensions of the finest resolution
    Eigen::RowVector3d meshBoxDim(3.5 * params.scale, 3.5 * params.scale, 3.5 * params.scale);
    Eigen::RowVector3d meshBoxCenter(5 * params.scale, 5 * params.scale, 5 * params.scale);


    //read the mesh and scale it such that it fits inside meshBox
    Eigen::MatrixXi faces;
    Eigen::MatrixXd vertices;
    igl::read_triangle_mesh(params.meshFile, vertices, faces);

    //remove unreferenced vertices because they may affect the scaling
    Eigen::VectorXi _1, _2;
    igl::remove_unreferenced(Eigen::MatrixXd(vertices), Eigen::MatrixXi(faces), vertices, faces, _1, _2);


    //mesh bounding box using the mesh coordinates
    Eigen::RowVectorXd bbMax, bbMin;
    Eigen::RowVectorXi bbMaxI, bbMinI;
    igl::max(vertices, 1, bbMax, bbMaxI);
    igl::min(vertices, 1, bbMin, bbMinI);

    //center the mesh such that the center of its bounding box is now 0,0,0
    vertices.rowwise() -= ((bbMin + bbMax) / 2.0);

    //scale the mesh within the box that will encompass it
    double scaling_factor = meshBoxDim.maxCoeff() / (bbMax - bbMin).maxCoeff();
    vertices *= scaling_factor;

    auto toRad = [](double t) { return t * (3.14159265358979311600 / 180); };

    Eigen::Matrix3d rotationX;
    //rotate about x-axis by user-input thetaX degrees
    rotationX << 1, 0, 0,
        0, std::cos(toRad(params.thetaX)), -std::sin(toRad(params.thetaX)),
        0, std::sin(toRad(params.thetaX)), std::cos(toRad(params.thetaX));

    //rotate about y-axis by user-input thetaY degrees
    Eigen::Matrix3d rotationY;
    rotationY << std::cos(toRad(params.thetaY)), 0, std::sin(toRad(params.thetaY)),
        0, 1, 0,
        -std::sin(toRad(params.thetaY)), 0, std::cos(toRad(params.thetaY));

    //rotate about z-axis by user-input thetaZ degrees
    Eigen::Matrix3d rotationZ;
    rotationZ << std::cos(toRad(params.thetaZ)), -std::sin(toRad(params.thetaZ)), 0,
        std::sin(toRad(params.thetaZ)), std::cos(toRad(params.thetaZ)), 0,
        0, 0, 1;

    Eigen::Matrix3d rotation = rotationX * rotationY * rotationZ;
    vertices = vertices * rotation;

    //translate
    vertices.rowwise() += meshBoxCenter;

    //compute the new bounding box
    igl::max(vertices, 1, bbMax, bbMaxI);
    igl::min(vertices, 1, bbMin, bbMinI);

    //write the final mesh in obj (to be combined later with VTK files in e.g., paraview)
    igl::writeOBJ("scaled.obj", vertices, faces);

#ifdef NEON_USE_POLYSCOPE
    polyscopeAddMesh(params.meshFile, faces, vertices);
#endif

    NEON_INFO("AABB init");

    //initialize the AABB that is used to speed up the inside/output calculation for the grid construction
    igl::AABB<Eigen::MatrixXd, 3> aabb;
    aabb.init(vertices, faces);


    //Define the multi-resolution grid. depth is the number of layers/resolutions in the grid
    int depth = 3;

    //helper function that could be used to make the grid more aligned with the input geometry
    auto distToMesh = [&](const Neon::index_3d idx) {
        Eigen::Matrix<double, 1, 3> p(idx.x, idx.y, idx.z), c;
        int                         face_index(0);
        aabb.squared_distance(vertices, faces, p, face_index, c);
        return (p - c).norm();
    };

    //Activation function for the different resolution. For each layer in the grid, we need to specify which
    //voxels is active. Internally, Neon goes over each voxel at each level and check if it active or not using this
    //activate function. The first lambda function corresponds to the finest level, the second is for the next coarse level,
    // and so on.
    std::vector<std::function<bool(const Neon::index_3d&)>>
        activeCellLambda =
            {[&](const Neon::index_3d idx) -> bool {
                 return idx.x >= 2 * params.scale && idx.x < 8 * params.scale &&
                        idx.y >= 3 * params.scale && idx.y < 7 * params.scale &&
                        idx.z >= 3 * params.scale && idx.z < 7 * params.scale;
                 //return distToMesh(idx) < params.scale;
             },
             [&](const Neon::index_3d idx) -> bool {
                 return idx.x >= params.scale && idx.x < 13 * params.scale &&
                        idx.y >= 2 * params.scale && idx.y < 8 * params.scale &&
                        idx.z >= 2 * params.scale && idx.z < 8 * params.scale;
                 //return distToMesh(idx) < 2 * params.scale;
             },
             [&](const Neon::index_3d idx) -> bool {
                 return true;
             }};

    //int depth = 5;
    //
    //std::vector<std::function<bool(const Neon::index_3d&)>>
    //    activeCellLambda =
    //        {[&](const Neon::index_3d idx) -> bool {
    //             return idx.x >= (0.1 * 29 * params.scale) + 4.3 * params.scale && idx.x < (0.1 * 29 * params.scale) + 10 * params.scale &&
    //                    idx.y < 1.2 * params.scale &&
    //                    idx.z >= 3 * params.scale && idx.z < 7 * params.scale;
    //         },
    //         [&](const Neon::index_3d idx) -> bool {
    //             return idx.x >= (0.1 * 29 * params.scale) + 3.0 * params.scale && idx.x < (0.1 * 29 * params.scale) + 14 * params.scale &&
    //                    idx.y < 3 * params.scale &&
    //                    idx.z >= 3 * params.scale && idx.z < 7 * params.scale;
    //         },
    //         [&](const Neon::index_3d idx) -> bool {
    //             return idx.x >= (0.1 * 29 * params.scale) + 2.2 * params.scale && idx.x < (0.1 * 29 * params.scale) + 19.5 * params.scale &&
    //                    idx.y < 4.3 * params.scale &&
    //                    idx.z >= 3 * params.scale && idx.z < 7 * params.scale;
    //         },
    //         [&](const Neon::index_3d idx) -> bool {
    //             return idx.x >= (0.1 * 29 * params.scale) + 0.7 * params.scale && idx.x < (0.1 * 29 * params.scale) + 25 * params.scale &&
    //                    idx.y < 6.2 * params.scale &&
    //                    idx.z >= 2 * params.scale && idx.z < 8 * params.scale;
    //         },
    //         [&](const Neon::index_3d idx) -> bool {
    //             return true;
    //         }};

    //Pass everything to Neon to build the multi-resolution grid
    NEON_INFO("Create mGrid");

    const Neon::mGridDescriptor<1> descriptor(depth);

    Neon::domain::mGrid grid(
        backend, gridDim, activeCellLambda, Neon::domain::Stencil::s19_t(false), descriptor);


    //LBM paramters
    const T uin = 0.04;  //input velocity
    //const T clength = T((meshBoxDim.minCoeff() / 2) / (1 << (depth - 1)));
    //the plane wing extends along the z direction
    const T               span = (bbMax - bbMin).z();
    const T               clength = (span / 2.f) / (1 << (depth - 1));
    const T               visclb = uin * clength / static_cast<T>(params.Re);
    const T               omega = 1.0 / (3. * visclb + 0.5);
    const Neon::double_3d inletVelocity(uin, 0., 0.);


    NEON_INFO("Started populating inside field");

    //Populate the field that specifies which voxel is inside/outside the input geometry.
    //In this field, 1 means the voxel is inside the shape
    auto inside = grid.newField<int8_t>("inside", 1, 0);

    for (int l = 0; l < grid.getDescriptor().getDepth(); ++l) {
        grid.newContainer<Neon::Execution::host>("isInside", l, [&](Neon::set::Loader& loader) {
                auto& in = inside.load(loader, l, Neon::MultiResCompute::MAP);

                return [&](const typename Neon::domain::mGrid::Idx& cell) mutable {
                    if (!in.hasChildren(cell) && l == 0) {
                        Neon::index_3d voxelGlobalLocation = in.getGlobalIndex(cell);
                        if (voxelGlobalLocation.x < meshBoxCenter.x() - meshBoxDim.x() / 2 || voxelGlobalLocation.x >= meshBoxCenter.x() + meshBoxDim.x() / 2 ||
                            voxelGlobalLocation.y < meshBoxCenter.y() - meshBoxDim.y() / 2 || voxelGlobalLocation.y >= meshBoxCenter.y() + meshBoxDim.y() / 2 ||
                            voxelGlobalLocation.z < meshBoxCenter.z() - meshBoxDim.z() / 2 || voxelGlobalLocation.z >= meshBoxCenter.z() + meshBoxDim.z() / 2) {
                            in(cell, 0) = 0;
                        } else {
                            const double voxelSpacing = 0.5 * double(grid.getDescriptor().getSpacing(l - 1));

                            const double centerToCornerDistSqr = (3.0 / 4.0) * (2.0 * voxelSpacing);

                            Eigen::Matrix<double, 1, 3> p(voxelGlobalLocation.x + voxelSpacing, voxelGlobalLocation.y + voxelSpacing, voxelGlobalLocation.z + voxelSpacing), c;

                            int face_index(0);

                            aabb.squared_distance(vertices, faces, p, face_index, c);

                            double sqr_dist = (p - c).norm();

                            in(cell, 0) = int8_t(sqr_dist + std::numeric_limits<double>::epsilon() < centerToCornerDistSqr);
                        }
                    } else {
                        in(cell, 0) = 0;
                    }
                };
            })
            .run(0);
    }
    grid.getBackend().syncAll();

    NEON_INFO("Finished populating inside field");

    inside.updateDeviceData();

    //inside.ioToVtk("inside", true, true, true, true);

    NEON_INFO("Start allocating fields");
    //allocate fields
    auto fin = grid.newField<T>("fin", Q, 0);
    auto fout = grid.newField<T>("fout", Q, 0);
    auto storeSum = grid.newField<float>("storeSum", Q, 0);
    auto cellType = grid.newField<CellType>("CellType", 1, CellType::bulk);

    NEON_INFO("Finished allocating fields");

    //init fields

    NEON_INFO("Start initFlowOverShape");
    initFlowOverShape<T, Q>(grid, storeSum, fin, fout, cellType, inletVelocity, inside);
    NEON_INFO("Finished initFlowOverShape");

    //Finally run the simulation
    runNonUniformLBM<T, Q>(grid,
                           params,
                           clength,
                           omega,
                           visclb,
                           inletVelocity,
                           cellType,
                           storeSum,
                           fin,
                           fout);
}