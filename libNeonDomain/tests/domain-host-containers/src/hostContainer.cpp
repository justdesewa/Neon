#include <functional>
#include "Neon/domain/Grids.h"

#include "Neon/domain/tools/TestData.h"
#include "TestInformation.h"
#include "gtest/gtest.h"


namespace hostContainer {

template <typename Field>
auto defContainer(Field& filedA,
                  Field& filedB,
                  Field& filedC)
    -> Neon::set::Container
{
    const auto& grid = filedA.getGrid();
    return grid.template newContainer<Neon::Execution::host>(
        "defContainer",
        [&](Neon::set::Loader& loader) {
           auto a = loader.load(filedA);
            auto b = loader.load(filedB);
            auto c = loader.load(filedC);

            return [=](const typename Field::Idx& e ) mutable {
                // printf("GPU %ld <- %ld + %ld\n", lc(e, i) , la(e, i) , val);
                Neon::index_3d globalPoint = a.getGlobalIndex(e);
                a(e, 0) = static_cast<typename Field::Type>(globalPoint.x);
                b(e, 0) = static_cast<typename Field::Type>(globalPoint.y);
                c(e, 0) = static_cast<typename Field::Type>(globalPoint.z);
                //                if constexpr (std::is_same_v<typename Field::Grid, Neon::bGrid>) {
                //                    printf("Block %d Th %d %d %d Loc %d %d %d\n", e.mDataBlockIdx,
                //                           e.mInDataBlockIdx.x,
                //                           e.mInDataBlockIdx.y,
                //                           e.mInDataBlockIdx.z,
                //                           globalPoint.x,
                //                           globalPoint.y,
                //                           globalPoint.z);
                //                }
            };
        });
}

using namespace Neon::domain::tool::testing;

template <typename G, typename T, int C>
auto run(TestData<G, T, C>& data) -> void
{

    using Type = typename TestData<G, T, C>::Type;
    auto&             grid = data.getGrid();
    const std::string appName = TestInformation::fullName(grid.getImplementationName());

    data.resetValuesToLinear(1, 100);

    {  // NEON
        const Neon::index_3d        dim = grid.getDimension();
        std::vector<Neon::index_3d> elements;

        auto& X = data.getField(FieldNames::X);
        auto& Y = data.getField(FieldNames::Y);
        auto& Z = data.getField(FieldNames::Z);

        defContainer(X, Y, Z)
            .run(Neon::Backend::mainStreamIdx);

        X.updateDeviceData(0);
        Y.updateDeviceData(0);
        Z.updateDeviceData(0);

        data.getBackend().sync(0);
    }

    {  // Golden data
        auto& X = data.getIODomain(FieldNames::X);
        auto& Y = data.getIODomain(FieldNames::Y);
        auto& Z = data.getIODomain(FieldNames::Z);

        data.forEachActiveIODomain([&](const Neon::index_3d& idx,
                                       int,
                                       Type& a,
                                       Type& b,
                                       Type& c) {
            a = idx.x;
            b = idx.y;
            c = idx.z;
        },
                                   X, Y, Z);
    }

    bool isOk = data.compare(FieldNames::X);
    isOk = isOk && data.compare(FieldNames::Y);
    isOk = isOk && data.compare(FieldNames::Z);

    ASSERT_TRUE(isOk);
}

template auto run<Neon::dGrid, int64_t, 0>(TestData<Neon::dGrid, int64_t, 0>&) -> void;
template auto run<Neon::eGrid, int64_t, 0>(TestData<Neon::eGrid, int64_t, 0>&) -> void;
template auto run<Neon::bGrid, int64_t, 0>(TestData<Neon::bGrid, int64_t, 0>&) -> void;

}  // namespace hostContainer