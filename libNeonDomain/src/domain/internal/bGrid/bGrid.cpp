#include "Neon/domain/internal/bGrid/bGrid.h"
#include "Neon/domain/interface/KernelConfig.h"
#include "Neon/domain/internal/bGrid/bPartitionIndexSpace.h"

namespace Neon::domain::internal::bGrid {

bGrid::bGrid(const Neon::Backend&                                    backend,
             const Neon::int32_3d&                                   domainSize,
             std::vector<std::function<bool(const Neon::index_3d&)>> activeCellLambda,
             const Neon::domain::Stencil&                            stencil,
             const bGridDescriptor                                   descriptor,
             const double_3d&                                        spacingData,
             const double_3d&                                        origin)
{


    if (backend.devSet().setCardinality() > 1) {
        NeonException exp("bGrid");
        exp << "bGrid only supported on a single GPU";
        NEON_THROW(exp);
    }

    mData = std::make_shared<Data>();

    mData->mStrongBalanced = true;

    mData->mNoPartialRefined = false;

    mData->descriptor.resize(descriptor.getDepth());
    int top_level_spacing = 1;
    for (int l = 0; l < descriptor.getDepth(); ++l) {
        mData->descriptor[l] = descriptor.getLevelRefFactor(l);
        if (l > 0) {
            top_level_spacing *= mData->descriptor[l];
            if (mData->descriptor[l] < mData->descriptor[l - 1]) {
                NeonException exp("bGrid::bGrid");
                exp << "The grid refinement factor should only go up from one level to another starting with Level 0 the leaf/finest level\n";
                exp << "Level " << l - 1 << " refinement factor= " << mData->descriptor[l - 1] << "\n";
                exp << "Level " << l << " refinement factor= " << mData->descriptor[l] << "\n";
                NEON_THROW(exp);
            }
        }
    }

    if (domainSize.x < top_level_spacing || domainSize.y < top_level_spacing || domainSize.z < top_level_spacing) {
        NeonException exp("bGrid::bGrid");
        exp << "The spacing of the top level of the multi-resolution grid is bigger than the domain size";
        exp << " This may create problems. Please consider increasing the domain size or decrease the branching factor or depth of the grid\n";
        exp << "DomainSize= " << domainSize << "\n";
        exp << "Top level spacing= " << top_level_spacing << "\n";
        NEON_THROW(exp);
    }


    mData->mBlockOriginTo1D.resize(descriptor.getDepth());
    mData->mBlockOriginTo1D[0] = Neon::domain::tool::PointHashTable<int32_t, uint32_t>(domainSize);

    mData->mNumBlocks.resize(descriptor.getDepth());
    for (auto& nb : mData->mNumBlocks) {
        nb = backend.devSet().template newDataSet<uint64_t>();
    }

    mData->mNumActiveVoxel.resize(descriptor.getDepth());
    for (auto& av : mData->mNumActiveVoxel) {
        av = backend.devSet().template newDataSet<uint64_t>();
    }
    mData->mNumActiveVoxel[0][0] = 0;


    std::vector<Neon::int32_3d> numBlockInDomain(descriptor.getDepth());
    numBlockInDomain[0].set(NEON_DIVIDE_UP(domainSize.x, descriptor.get0LevelRefFactor()),
                            NEON_DIVIDE_UP(domainSize.y, descriptor.get0LevelRefFactor()),
                            NEON_DIVIDE_UP(domainSize.z, descriptor.get0LevelRefFactor()));


    for (int i = 0; i < descriptor.getDepth(); ++i) {
        if (i > 0) {

            mData->mNumActiveVoxel[i][0] = 0;

            numBlockInDomain[i].set(NEON_DIVIDE_UP(numBlockInDomain[i - 1].x, descriptor.getLevelRefFactor(i)),
                                    NEON_DIVIDE_UP(numBlockInDomain[i - 1].y, descriptor.getLevelRefFactor(i)),
                                    NEON_DIVIDE_UP(numBlockInDomain[i - 1].z, descriptor.getLevelRefFactor(i)));

            mData->mBlockOriginTo1D[i] = Neon::domain::tool::PointHashTable<int32_t, uint32_t>(domainSize);
        }
        std::vector<uint32_t> msk(NEON_DIVIDE_UP(descriptor.getLevelRefFactor(i) * descriptor.getLevelRefFactor(i) * descriptor.getLevelRefFactor(i) *
                                                     numBlockInDomain[i].x * numBlockInDomain[i].y * numBlockInDomain[i].z,
                                                 Cell::sMaskSize),
                                  0);
        mData->denseLevelsBitmask.push_back(msk);
    }


    auto flattened1DIndex = [&](int l, int blockX, int blockY, int blockZ, int localX, int localY, int localZ) {
        int ref_factor = descriptor.getLevelRefFactor(l);

        int blockID = blockX +
                      blockY * numBlockInDomain[l].x +
                      blockZ * numBlockInDomain[l].x * numBlockInDomain[l].y;

        int id = localX +
                 localY * ref_factor +
                 localZ * ref_factor * ref_factor +
                 blockID * ref_factor * ref_factor * ref_factor;

        return id;
    };

    auto levelBitMaskIsSet = [&](int l, int blockX, int blockY, int blockZ, int localX, int localY, int localZ) {
        const int Index1D = flattened1DIndex(l, blockX, blockY, blockZ, localX, localY, localZ);
        const int mask = Index1D / Cell::sMaskSize;
        const int bitPosition = Index1D % Cell::sMaskSize;
        return mData->denseLevelsBitmask[l][mask] & (1 << bitPosition);
    };

    auto setLevelBitMask = [&](int l, int blockX, int blockY, int blockZ, int localX, int localY, int localZ) {
        const int Index1D = flattened1DIndex(l, blockX, blockY, blockZ, localX, localY, localZ);
        const int mask = Index1D / Cell::sMaskSize;
        const int bitPosition = Index1D % Cell::sMaskSize;
        mData->denseLevelsBitmask[l][mask] |= (1 << bitPosition);
    };

    //Each block loops over its voxels and check the lambda function and activate its voxels correspondingly
    //If a block contain an active voxel, it activates itself as well
    //This loop only sets the bitmask
    for (int l = 0; l < descriptor.getDepth(); ++l) {
        const int ref_factor = descriptor.getLevelRefFactor(l);
        const int ref_factor_recurse = descriptor.getRefFactorRecurse(l);
        const int prv_ref_factor_recurse = descriptor.getRefFactorRecurse(l - 1);

        for (int bz = 0; bz < numBlockInDomain[l].z; bz++) {
            for (int by = 0; by < numBlockInDomain[l].y; by++) {
                for (int bx = 0; bx < numBlockInDomain[l].x; bx++) {

                    Neon::int32_3d blockOrigin(bx * ref_factor_recurse,
                                               by * ref_factor_recurse,
                                               bz * ref_factor_recurse);

                    bool containVoxels = false;
                    for (int z = 0; z < ref_factor; z++) {
                        for (int y = 0; y < ref_factor; y++) {
                            for (int x = 0; x < ref_factor; x++) {

                                const Neon::int32_3d voxel(blockOrigin.x + x * prv_ref_factor_recurse,
                                                           blockOrigin.y + y * prv_ref_factor_recurse,
                                                           blockOrigin.z + z * prv_ref_factor_recurse);

                                if (voxel < domainSize) {
                                    //if it is already active
                                    if (levelBitMaskIsSet(l, bx, by, bz, x, y, z)) {
                                        containVoxels = true;
                                    } else {
                                        if (activeCellLambda[l](voxel)) {
                                            containVoxels = true;
                                            setLevelBitMask(l, bx, by, bz, x, y, z);
                                        }
                                    }
                                }
                            }
                        }
                    }


                    if (containVoxels) {
                        //if the block contains voxels, it should activate itself
                        //find its corresponding index within the next level

                        if (l < descriptor.getDepth() - 1) {
                            const int nxt_ref_factor = descriptor.getLevelRefFactor(l + 1);

                            Neon::int32_3d parentBlock(bx / nxt_ref_factor,
                                                       by / nxt_ref_factor,
                                                       bz / nxt_ref_factor);

                            Neon::int32_3d indexInParentBlock(bx % nxt_ref_factor,
                                                              by % nxt_ref_factor,
                                                              bz % nxt_ref_factor);

                            setLevelBitMask(l + 1,
                                            parentBlock.x, parentBlock.y, parentBlock.z,
                                            indexInParentBlock.x, indexInParentBlock.y, indexInParentBlock.z);
                        }
                    }
                }
            }
        }
    }

    //Impose the strong balance condition
    if (mData->mStrongBalanced) {
        bool again = true;
        while (again) {
            again = false;


            for (int l = 0; l < descriptor.getDepth(); ++l) {
                const int ref_factor = descriptor.getLevelRefFactor(l);
                const int prv_ref_factor_recurse = descriptor.getRefFactorRecurse(l - 1);

                for (int bz = 0; bz < numBlockInDomain[l].z; bz++) {
                    for (int by = 0; by < numBlockInDomain[l].y; by++) {
                        for (int bx = 0; bx < numBlockInDomain[l].x; bx++) {


                            for (int z = 0; z < ref_factor; z++) {
                                for (int y = 0; y < ref_factor; y++) {
                                    for (int x = 0; x < ref_factor; x++) {

                                        const Neon::int32_3d voxel(bx * ref_factor + x,
                                                                   by * ref_factor + y,
                                                                   bz * ref_factor + z);


                                        //if this voxel is active
                                        if (levelBitMaskIsSet(l, bx, by, bz, x, y, z)) {

                                            for (int k = -1; k < 2; k++) {
                                                for (int j = -1; j < 2; j++) {
                                                    for (int i = -1; i < 2; i++) {
                                                        if (i == 0 && j == 0 && k == 0) {
                                                            continue;
                                                        }

                                                        Neon::int32_3d proxyVoxel(voxel.x + i,
                                                                                  voxel.y + j,
                                                                                  voxel.z + k);

                                                        const Neon::int32_3d proxyVoxelLocation(proxyVoxel.x * prv_ref_factor_recurse,
                                                                                                proxyVoxel.y * prv_ref_factor_recurse,
                                                                                                proxyVoxel.z * prv_ref_factor_recurse);

                                                        if (proxyVoxelLocation < domainSize && proxyVoxelLocation >= 0) {

                                                            Neon::int32_3d prv_nVoxelBlockOrigin, prv_nVoxelLocalID;
                                                            for (int l_n = l; l_n < descriptor.getDepth(); ++l_n) {
                                                                const int l_n_ref_factor = descriptor.getLevelRefFactor(l_n);


                                                                //find the block origin of n_voxel which live at level l_n
                                                                const Neon::int32_3d nVoxelBlockOrigin(proxyVoxel.x / l_n_ref_factor,
                                                                                                       proxyVoxel.y / l_n_ref_factor,
                                                                                                       proxyVoxel.z / l_n_ref_factor);

                                                                const Neon::int32_3d nVoxelLocalID(proxyVoxel.x % l_n_ref_factor,
                                                                                                   proxyVoxel.y % l_n_ref_factor,
                                                                                                   proxyVoxel.z % l_n_ref_factor);

                                                                //find if this block origin is active
                                                                if (levelBitMaskIsSet(l_n,
                                                                                      nVoxelBlockOrigin.x, nVoxelBlockOrigin.y, nVoxelBlockOrigin.z,
                                                                                      nVoxelLocalID.x, nVoxelLocalID.y, nVoxelLocalID.z)) {

                                                                    //if this neighbor is at the same level or +1 level, then there is nothing else we should check on
                                                                    if (l_n == l || l_n == l + 1) {
                                                                        break;
                                                                    } else {
                                                                        //otherwise, we should refine the previous block and voxel

                                                                        setLevelBitMask(l_n - 1,
                                                                                        prv_nVoxelBlockOrigin.x, prv_nVoxelBlockOrigin.y, prv_nVoxelBlockOrigin.z,
                                                                                        prv_nVoxelLocalID.x, prv_nVoxelLocalID.y, prv_nVoxelLocalID.z);

                                                                        again = true;
                                                                    }
                                                                }

                                                                //promote the proxy voxel to the next level
                                                                proxyVoxel = nVoxelBlockOrigin;

                                                                //cache the voxel and block at this level because we might need to activate them
                                                                prv_nVoxelBlockOrigin = nVoxelBlockOrigin;
                                                                prv_nVoxelLocalID = nVoxelLocalID;
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }


    // Number of active voxels per partition
    // Loop over all blocks and voxels in blocks to count the number of active
    // voxels and active blocks for allocation
    for (int l = 0; l < descriptor.getDepth(); ++l) {
        const int ref_factor = descriptor.getLevelRefFactor(l);
        const int ref_factor_recurse = descriptor.getRefFactorRecurse(l);

        for (int bz = 0; bz < numBlockInDomain[l].z; bz++) {
            for (int by = 0; by < numBlockInDomain[l].y; by++) {
                for (int bx = 0; bx < numBlockInDomain[l].x; bx++) {

                    Neon::int32_3d blockOrigin(bx * ref_factor_recurse,
                                               by * ref_factor_recurse,
                                               bz * ref_factor_recurse);

                    int numVoxelsInBlock = 0;

                    for (int z = 0; z < ref_factor; z++) {
                        for (int y = 0; y < ref_factor; y++) {
                            for (int x = 0; x < ref_factor; x++) {

                                if (levelBitMaskIsSet(l, bx, by, bz, x, y, z)) {
                                    numVoxelsInBlock++;
                                }
                            }
                        }
                    }

                    //if we don't accept partially refined blocks, if there is one voxel in the block that is active
                    //then all voxels are active in this block
                    if (mData->mNoPartialRefined && numVoxelsInBlock > 0 && l == 0) {
                        numVoxelsInBlock = ref_factor * ref_factor * ref_factor;
                    }
                    mData->mNumActiveVoxel[l][0] += numVoxelsInBlock;


                    if (numVoxelsInBlock > 0) {
                        mData->mNumBlocks[l][0]++;
                        mData->mBlockOriginTo1D[l].addPoint(blockOrigin, uint32_t(mData->mBlockOriginTo1D[l].size()));
                    }
                }
            }
        }
    }


    // Init the base grid
    bGrid::GridBase::init("bGrid",
                          backend,
                          domainSize,
                          Neon::domain::Stencil(),
                          mData->mNumActiveVoxel[0],  //passing active voxels on level 0 as the number of active grid in base grid (????)
                          Neon::int32_3d(Cell::sBlockSizeX, Cell::sBlockSizeY, Cell::sBlockSizeZ),
                          spacingData,
                          origin);


    Neon::MemoryOptions memOptions(Neon::DeviceType::CPU,
                                   Neon::Allocator::MALLOC,
                                   Neon::DeviceType::CUDA,
                                   ((backend.devType() == Neon::DeviceType::CUDA) ? Neon::Allocator::CUDA_MEM_DEVICE : Neon::Allocator::NULL_MEM),
                                   Neon::MemoryLayout::arrayOfStructs);

    mData->mOrigin.resize(descriptor.getDepth());
    for (int l = 0; l < descriptor.getDepth(); ++l) {
        mData->mOrigin[l] = backend.devSet().template newMemSet<Neon::int32_3d>({Neon::DataUse::IO_COMPUTE},
                                                                                1,
                                                                                memOptions,
                                                                                mData->mNumBlocks[l]);
    }

    //Stencil linear/relative index
    auto stencilNghSize = backend.devSet().template newDataSet<uint64_t>();
    for (int32_t c = 0; c < stencilNghSize.cardinality(); ++c) {
        stencilNghSize[c] = stencil.neighbours().size();
    }
    mData->mStencilNghIndex = backend.devSet().template newMemSet<nghIdx_t>({Neon::DataUse::IO_COMPUTE},
                                                                            1,
                                                                            memOptions,
                                                                            stencilNghSize);
    for (int32_t c = 0; c < mData->mStencilNghIndex.cardinality(); ++c) {
        SetIdx devID(c);
        for (uint64_t s = 0; s < stencil.neighbours().size(); ++s) {
            mData->mStencilNghIndex.eRef(c, s).x = static_cast<nghIdx_t::Integer>(stencil.neighbours()[s].x);
            mData->mStencilNghIndex.eRef(c, s).y = static_cast<nghIdx_t::Integer>(stencil.neighbours()[s].y);
            mData->mStencilNghIndex.eRef(c, s).z = static_cast<nghIdx_t::Integer>(stencil.neighbours()[s].z);
        }
    }


    // block bitmask
    mData->mActiveMaskSize.resize(descriptor.getDepth());
    mData->mActiveMask.resize(descriptor.getDepth());
    for (int l = 0; l < descriptor.getDepth(); ++l) {
        mData->mActiveMaskSize[l] = backend.devSet().template newDataSet<uint64_t>();
        for (int64_t i = 0; i < mData->mActiveMaskSize[l].size(); ++i) {
            mData->mActiveMaskSize[l][i] = mData->mNumBlocks[l][i] *
                                           NEON_DIVIDE_UP(descriptor.getLevelRefFactor(l) * descriptor.getLevelRefFactor(l) * descriptor.getLevelRefFactor(l),
                                                          Cell::sMaskSize);
        }

        mData->mActiveMask[l] = backend.devSet().template newMemSet<uint32_t>({Neon::DataUse::IO_COMPUTE},
                                                                              1,
                                                                              memOptions,
                                                                              mData->mActiveMaskSize[l]);
    }


    // init bitmask to zero
    for (int l = 0; l < descriptor.getDepth(); ++l) {
        for (int32_t c = 0; c < mData->mActiveMask[l].cardinality(); ++c) {
            SetIdx devID(c);
            for (size_t i = 0; i < mData->mActiveMaskSize[l][c]; ++i) {
                mData->mActiveMask[l].eRef(devID, i) = 0;
            }
        }
    }


    // Neighbor blocks
    mData->mNeighbourBlocks.resize(descriptor.getDepth());
    for (int l = 0; l < descriptor.getDepth(); ++l) {
        mData->mNeighbourBlocks[l] = backend.devSet().template newMemSet<uint32_t>({Neon::DataUse::IO_COMPUTE},
                                                                                   26,
                                                                                   memOptions,
                                                                                   mData->mNumBlocks[l]);
        // init neighbor blocks to invalid block id
        for (int32_t c = 0; c < mData->mNeighbourBlocks[l].cardinality(); ++c) {
            SetIdx devID(c);
            for (uint64_t i = 0; i < mData->mNumBlocks[l][c]; ++i) {
                for (int n = 0; n < 26; ++n) {
                    mData->mNeighbourBlocks[l].eRef(devID, i, n) = std::numeric_limits<uint32_t>::max();
                }
            }
        }
    }


    // loop over active blocks to populate the block origins, neighbors, and bitmask
    for (int l = 0; l < descriptor.getDepth(); ++l) {
        const int ref_factor = descriptor.getLevelRefFactor(l);
        const int ref_factor_recurse = descriptor.getRefFactorRecurse(l);

        mData->mBlockOriginTo1D[l].forEach([&](const Neon::int32_3d blockOrigin, const uint32_t blockIdx) {
            // TODO need to figure out which device owns this block
            SetIdx devID(0);

            mData->mOrigin[l].eRef(devID, blockIdx) = blockOrigin;

            Neon::int32_3d block3DIndex = blockOrigin / ref_factor_recurse;

            //set active mask
            std::vector<Cell::Location> activeVoxelsInBlock;

            auto setCellActiveMask = [&](Cell::Location::Integer x, Cell::Location::Integer y, Cell::Location::Integer z) {
                Cell cell(x, y, z);
                cell.mBlockID = blockIdx;
                mData->mActiveMask[l].eRef(devID, cell.getBlockMaskStride(ref_factor) + cell.getMaskLocalID(ref_factor), 0) |= 1 << cell.getMaskBitPosition(ref_factor);
            };

            for (Cell::Location::Integer z = 0; z < ref_factor; z++) {
                for (Cell::Location::Integer y = 0; y < ref_factor; y++) {
                    for (Cell::Location::Integer x = 0; x < ref_factor; x++) {

                        //store the local index of the active voxel
                        if (levelBitMaskIsSet(l, block3DIndex.x, block3DIndex.y, block3DIndex.z, x, y, z)) {
                            activeVoxelsInBlock.push_back({x, y, z});
                        }
                    }
                }
            }


            //if partially refined block is not allowed and we have one voxel active in the block,
            //then we should active all voxels in this block
            if (mData->mNoPartialRefined && !activeVoxelsInBlock.empty()) {
                for (Cell::Location::Integer z = 0; z < ref_factor; z++) {
                    for (Cell::Location::Integer y = 0; y < ref_factor; y++) {
                        for (Cell::Location::Integer x = 0; x < ref_factor; x++) {
                            setCellActiveMask(x, y, z);
                        }
                    }
                }

            } else {
                //otherwise we only add these voxels that are active
                for (auto& voxel : activeVoxelsInBlock) {
                    setCellActiveMask(voxel.x, voxel.y, voxel.z);
                }
            }


            //set neighbor blocks
            for (int16_t k = -1; k < 2; k++) {
                for (int16_t j = -1; j < 2; j++) {
                    for (int16_t i = -1; i < 2; i++) {
                        if (i == 0 && j == 0 && k == 0) {
                            continue;
                        }

                        Neon::int32_3d neighbourBlockOrigin(i, j, k);
                        if (l == 0) {
                            neighbourBlockOrigin.x = neighbourBlockOrigin.x * ref_factor + blockOrigin.x;
                            neighbourBlockOrigin.y = neighbourBlockOrigin.y * ref_factor + blockOrigin.y;
                            neighbourBlockOrigin.z = neighbourBlockOrigin.z * ref_factor + blockOrigin.z;
                        } else {
                            neighbourBlockOrigin.x = neighbourBlockOrigin.x * ref_factor_recurse + blockOrigin.x;
                            neighbourBlockOrigin.y = neighbourBlockOrigin.y * ref_factor_recurse + blockOrigin.y;
                            neighbourBlockOrigin.z = neighbourBlockOrigin.z * ref_factor_recurse + blockOrigin.z;
                        }

                        if (neighbourBlockOrigin >= 0 && neighbourBlockOrigin < domainSize) {

                            auto neighbour_it = mData->mBlockOriginTo1D[l].getMetadata(neighbourBlockOrigin);

                            if (neighbour_it) {
                                int16_3d block_offset(i, j, k);
                                mData->mNeighbourBlocks[l].eRef(devID,
                                                                blockIdx,
                                                                Cell::getNeighbourBlockID(block_offset)) = *neighbour_it;
                            }
                        }
                    }
                }
            }
        });
    }

    if (backend.devType() == Neon::DeviceType::CUDA) {
        for (int l = 0; l < descriptor.getDepth(); ++l) {
            mData->mActiveMask[l].updateCompute(backend, 0);
            mData->mOrigin[l].updateCompute(backend, 0);
            mData->mNeighbourBlocks[l].updateCompute(backend, 0);
        }
        mData->mStencilNghIndex.updateCompute(backend, 0);
    }


    mData->mPartitionIndexSpace = std::vector<Neon::set::DataSet<PartitionIndexSpace>>(3);

    for (const auto& dv : {Neon::DataView::STANDARD,
                           Neon::DataView::INTERNAL,
                           Neon::DataView::BOUNDARY}) {

        int dv_id = DataViewUtil::toInt(dv);
        if (dv_id > 2) {
            NeonException exp("bGrid");
            exp << "Inconsistent enumeration for DataView_t";
            NEON_THROW(exp);
        }

        mData->mPartitionIndexSpace[dv_id] = backend.devSet().template newDataSet<PartitionIndexSpace>();

        for (int gpuIdx = 0; gpuIdx < backend.devSet().setCardinality(); gpuIdx++) {
            mData->mPartitionIndexSpace[dv_id][gpuIdx].mDataView = dv;
            mData->mPartitionIndexSpace[dv_id][gpuIdx].mDomainSize = domainSize;
            mData->mPartitionIndexSpace[dv_id][gpuIdx].mNumBlocks = static_cast<uint32_t>(mData->mNumBlocks[0][gpuIdx]);
            mData->mPartitionIndexSpace[dv_id][gpuIdx].mHostActiveMask = mData->mActiveMask[0].rawMem(gpuIdx, Neon::DeviceType::CPU);
            mData->mPartitionIndexSpace[dv_id][gpuIdx].mDeviceActiveMask = mData->mActiveMask[0].rawMem(gpuIdx, Neon::DeviceType::CUDA);
            mData->mPartitionIndexSpace[dv_id][gpuIdx].mHostBlockOrigin = mData->mOrigin[0].rawMem(gpuIdx, Neon::DeviceType::CPU);
            mData->mPartitionIndexSpace[dv_id][gpuIdx].mDeviceBlockOrigin = mData->mOrigin[0].rawMem(gpuIdx, Neon::DeviceType::CUDA);
        }
    }
}

auto bGrid::getProperties(const Neon::index_3d& idx) const -> GridBaseTemplate::CellProperties
{
    GridBaseTemplate::CellProperties cellProperties;
    cellProperties.setIsInside(isInsideDomain(idx));
    if (!cellProperties.isInside()) {
        return cellProperties;
    }

    if (this->getDevSet().setCardinality() == 1) {
        cellProperties.init(0, DataView::INTERNAL);
    } else {
        //TODO
        NEON_DEV_UNDER_CONSTRUCTION("bGrid only support single GPU");
    }
    return cellProperties;
}

auto bGrid::isInsideDomain(const Neon::index_3d& idx) const -> bool
{
    if (this->getDevSet().setCardinality() != 1) {
        NEON_DEV_UNDER_CONSTRUCTION("bGrid only support single GPU");
    }

    //TODO need to figure out which device owns this block
    SetIdx devID(0);

    //We don't have to check over the domain bounds. If idx is outside the domain
    // (i.e., idx beyond the bounds of the domain) its block origin will be null

    Neon::int32_3d block_origin = getOriginBlock3DIndex(idx);

    auto itr = mData->mBlockOriginTo1D[0].getMetadata(block_origin);
    if (itr) {
        Cell cell(static_cast<Cell::Location::Integer>(idx.x % Cell::sBlockSizeX),
                  static_cast<Cell::Location::Integer>(idx.y % Cell::sBlockSizeY),
                  static_cast<Cell::Location::Integer>(idx.z % Cell::sBlockSizeZ));
        cell.mBlockID = *itr;
        cell.mIsActive = cell.computeIsActive(mData->mActiveMask[0].rawMem(devID, Neon::DeviceType::CPU));
        return cell.mIsActive;
    }
    return false;
}

auto bGrid::getOriginBlock3DIndex(const Neon::int32_3d idx) const -> Neon::int32_3d
{
    //round n to nearest multiple of m
    auto roundDownToNearestMultiple = [](int32_t n, int32_t m) -> int32_t {
        return (n / m) * m;
    };

    Neon::int32_3d block_origin(roundDownToNearestMultiple(idx.x, Cell::sBlockSizeX),
                                roundDownToNearestMultiple(idx.y, Cell::sBlockSizeY),
                                roundDownToNearestMultiple(idx.z, Cell::sBlockSizeZ));
    return block_origin;
}

auto bGrid::setReduceEngine(Neon::sys::patterns::Engine eng) -> void
{
    if (eng != Neon::sys::patterns::Engine::CUB) {
        NeonException exp("bGrid::setReduceEngine");
        exp << "bGrid only work on CUB engine for reduction";
        NEON_THROW(exp);
    }
}

auto bGrid::getLaunchParameters(Neon::DataView                         dataView,
                                [[maybe_unused]] const Neon::index_3d& blockSize,
                                const size_t&                          sharedMem) const -> Neon::set::LaunchParameters
{
    //TODO
    if (dataView != Neon::DataView::STANDARD) {
        NEON_WARNING("Requesting LaunchParameters on {} data view but bGrid only supports Standard data view on a single GPU",
                     Neon::DataViewUtil::toString(dataView));
    }
    const Neon::int32_3d        cuda_block(Cell::sBlockSizeX, Cell::sBlockSizeY, Cell::sBlockSizeZ);
    Neon::set::LaunchParameters ret = getBackend().devSet().newLaunchParameters();
    for (int i = 0; i < ret.cardinality(); ++i) {
        if (getBackend().devType() == Neon::DeviceType::CUDA) {
            ret[i].set(Neon::sys::GpuLaunchInfo::mode_e::cudaGridMode,
                       Neon::int32_3d(int32_t(mData->mNumBlocks[0][i]), 1, 1),
                       cuda_block, sharedMem);
        } else {
            ret[i].set(Neon::sys::GpuLaunchInfo::mode_e::domainGridMode,
                       Neon::int32_3d(int32_t(mData->mNumBlocks[0][i]) * Cell::sBlockSizeX * Cell::sBlockSizeY * Cell::sBlockSizeZ, 1, 1),
                       cuda_block, sharedMem);
        }
    }
    return ret;
}

auto bGrid::getPartitionIndexSpace(Neon::DeviceType dev,
                                   SetIdx           setIdx,
                                   Neon::DataView   dataView) -> const PartitionIndexSpace&
{
    return mData->mPartitionIndexSpace.at(Neon::DataViewUtil::toInt(dataView)).local(dev, setIdx, dataView);
}


auto bGrid::getNumBlocksPerPartition(int level) const -> const Neon::set::DataSet<uint64_t>&
{
    return mData->mNumBlocks[level];
}

auto bGrid::getOrigins(int level) const -> const Neon::set::MemSet_t<Neon::int32_3d>&
{
    return mData->mOrigin[level];
}

auto bGrid::getStencilNghIndex() const -> const Neon::set::MemSet_t<nghIdx_t>&
{
    return mData->mStencilNghIndex;
}

auto bGrid::getNeighbourBlocks(int level) const -> const Neon::set::MemSet_t<uint32_t>&
{
    return mData->mNeighbourBlocks[level];
}

auto bGrid::getActiveMask(int level) const -> const Neon::set::MemSet_t<uint32_t>&
{
    return mData->mActiveMask[level];
}

auto bGrid::getBlockOriginTo1D(int level) const -> const Neon::domain::tool::PointHashTable<int32_t, uint32_t>&
{
    return mData->mBlockOriginTo1D[level];
}

auto bGrid::getKernelConfig(int            streamIdx,
                            Neon::DataView dataView) -> Neon::set::KernelConfig
{
    Neon::domain::KernelConfig kernelConfig(streamIdx, dataView);
    if (kernelConfig.runtime() != Neon::Runtime::system) {
        NEON_DEV_UNDER_CONSTRUCTION("bGrid::getKernelConfig");
    }

    Neon::set::LaunchParameters launchInfoSet = getLaunchParameters(dataView,
                                                                    getDefaultBlock(), 0);

    kernelConfig.expertSetLaunchParameters(launchInfoSet);
    kernelConfig.expertSetBackend(getBackend());

    return kernelConfig;
}

auto bGrid::getDescriptor() const -> const std::vector<int>&
{
    return mData->descriptor;
}

void bGrid::topologyToVTK(std::string fileName) const
{

    std::ofstream file(fileName);
    file << "# vtk DataFile Version 2.0\n";
    file << "bGrid\n";
    file << "ASCII\n";
    file << "DATASET UNSTRUCTURED_GRID\n";
    file << "POINTS " << (getDimension().rMax() + 1) * (getDimension().rMax() + 1) * (getDimension().rMax() + 1) << " float \n";
    for (int z = 0; z < getDimension().rMax() + 1; ++z) {
        for (int y = 0; y < getDimension().rMax() + 1; ++y) {
            for (int x = 0; x < getDimension().rMax() + 1; ++x) {
                file << x << " " << y << " " << z << "\n";
            }
        }
    }

    uint64_t num_cells = 0;
    for (auto& a : mData->mNumActiveVoxel) {
        num_cells += a[0];
    }

    file << "CELLS " << num_cells << " " << num_cells * 9 << " \n";

    auto mapTo1D = [&](int x, int y, int z) {
        return x +
               y * (getDimension().rMax() + 1) +
               z * (getDimension().rMax() + 1) * (getDimension().rMax() + 1);
    };


    for (int l = 0; l < mData->descriptor.size(); ++l) {
        const int ref_factor = mData->descriptor[l];
        int       prv_ref_factor_recurse = 1;
        if (l > 0) {
            for (int ll = l - 1; ll >= 0; --ll) {
                prv_ref_factor_recurse *= mData->descriptor[ll];
            }
        }
        mData->mBlockOriginTo1D[l].forEach([&](const Neon::int32_3d blockOrigin, const uint32_t blockIdx) {
            // TODO need to figure out which device owns this block
            SetIdx devID(0);


            for (int z = 0; z < ref_factor; z++) {
                for (int y = 0; y < ref_factor; y++) {
                    for (int x = 0; x < ref_factor; x++) {
                        Cell cell(static_cast<Cell::Location::Integer>(x),
                                  static_cast<Cell::Location::Integer>(y),
                                  static_cast<Cell::Location::Integer>(z));
                        cell.mBlockID = blockIdx;

                        if (cell.computeIsActive(mData->mActiveMask[l].rawMem(devID, Neon::DeviceType::CPU), ref_factor)) {

                            Neon::int32_3d corner(blockOrigin.x + x * prv_ref_factor_recurse,
                                                  blockOrigin.y + y * prv_ref_factor_recurse,
                                                  blockOrigin.z + z * prv_ref_factor_recurse);


                            /*if (l > 0) {
                                //check if the voxel is refined 
                                const int prv_ref_factor = mData->descriptor[l - 1];
                                for (int zp = 0; zp < prv_ref_factor; ++zp) {
                                    for (int yp = 0; yp < prv_ref_factor; ++yp) {
                                        for (int xp = 0; xp < prv_ref_factor; ++xp) {
                                            Cell pCell(static_cast<Cell::Location::Integer>(xp),
                                                       static_cast<Cell::Location::Integer>(yp),
                                                       static_cast<Cell::Location::Integer>(zp));

                                            Neon::int32_3d pCorner(xp + corner.x,
                                                                   yp + corner.y,
                                                                   zp + corner.z);

                                            if (pCorner < getDimension()) {
                                                auto bOrigin = mData->mBlockOriginTo1D[l - 1].getMetadata(pCorner);
                                                if (bOrigin) {

                                                    pCell.mBlockID = *bOrigin;

                                                    if (pCell.computeIsActive(mData->mActiveMask[l - 1].rawMem(devID, Neon::DeviceType::CPU), prv_ref_factor)) {
                                                        isRefined = true;
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }*/


                            file << "8 ";
                            //x,y,z
                            file << mapTo1D(corner.x, corner.y, corner.z) << " ";
                            //+x,y,z
                            file << mapTo1D(corner.x + prv_ref_factor_recurse, corner.y, corner.z) << " ";

                            //x,+y,z
                            file << mapTo1D(corner.x, corner.y + prv_ref_factor_recurse, corner.z) << " ";

                            //+x,+y,z
                            file << mapTo1D(corner.x + prv_ref_factor_recurse, corner.y + prv_ref_factor_recurse, corner.z) << " ";

                            //x,y,+z
                            file << mapTo1D(corner.x, corner.y, corner.z + prv_ref_factor_recurse) << " ";

                            //+x,y,+z
                            file << mapTo1D(corner.x + prv_ref_factor_recurse, corner.y, corner.z + prv_ref_factor_recurse) << " ";

                            //x,+y,+z
                            file << mapTo1D(corner.x, corner.y + prv_ref_factor_recurse, corner.z + prv_ref_factor_recurse) << " ";

                            //+x,+y,+z
                            file << mapTo1D(corner.x + prv_ref_factor_recurse, corner.y + prv_ref_factor_recurse, corner.z + prv_ref_factor_recurse) << " ";
                            file << "\n";
                        }
                    }
                }
            }
        });
    }

    file << "CELL_TYPES " << num_cells << " \n";
    for (uint64_t i = 0; i < num_cells; ++i) {
        file << 11 << "\n";
    }

    file << "CELL_DATA " << num_cells << " \n";
    file << "SCALARS Level int 1 \n";
    file << "LOOKUP_TABLE default \n";

    uint64_t acc = 0;
    for (auto& a : mData->mNumActiveVoxel) {
        for (uint64_t i = 0; i < a[0]; ++i) {
            file << acc << "\n";
        }
        acc++;
    }

    file.close();
}


}  // namespace Neon::domain::internal::bGrid