#pragma once

#include "Neon/core/types/Macros.h"

enum CellType : int
{
    bounceBack = 0,
    movingWall = 1,
    bulk = 2,
    inlet = 3,
    undefined = 4,
};

#ifdef KBC
NEON_CUDA_DEVICE_ONLY static constexpr char latticeVelocity[27][3] = {
    {0, 0, 0},
    {0, 0, -1},
    {0, 0, 1},
    {0, -1, 0},
    {0, -1, -1},
    {0, -1, 1},
    {0, 1, 0},
    {0, 1, -1},
    {0, 1, 1},
    {-1, 0, 0},
    {-1, 0, -1},
    {-1, 0, 1},
    {-1, -1, 0},
    {-1, -1, -1},
    {-1, -1, 1},
    {-1, 1, 0},
    {-1, 1, -1},
    {-1, 1, 1},
    {1, 0, 0},
    {1, 0, -1},
    {1, 0, 1},
    {1, -1, 0},
    {1, -1, -1},
    {1, -1, 1},
    {1, 1, 0},
    {1, 1, -1},
    {1, 1, 1}};

NEON_CUDA_DEVICE_ONLY static constexpr char latticeOppositeID[27] = {
    0, 2, 1, 6, 8, 7, 3, 5, 4, 18, 20, 19, 24, 26, 25, 21, 23, 22, 9, 11, 10, 15, 17, 16, 12, 14, 13};

NEON_CUDA_DEVICE_ONLY static constexpr double latticeWeights[27] = {
    8.0 / 27.0,
    2.0 / 27.0,
    2.0 / 27.0,
    2.0 / 27.0,
    1.0 / 54.0,
    1.0 / 54.0,
    2.0 / 27.0,
    1.0 / 54.0,
    1.0 / 54.0,
    2.0 / 27.0,
    1.0 / 54.0,
    1.0 / 54.0,
    1.0 / 54.0,
    1.0 / 216.0,
    1.0 / 216.0,
    1.0 / 54.0,
    1.0 / 216.0,
    1.0 / 216.0,
    2.0 / 27.0,
    1.0 / 54.0,
    1.0 / 54.0,
    1.0 / 54.0,
    1.0 / 216.0,
    1.0 / 216.0,
    1.0 / 54.0,
    1.0 / 216.0,
    1.0 / 216.0

};
#endif

NEON_CUDA_DEVICE_ONLY static constexpr char latticeMoment[27][6] = {
    {0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 1},
    {0, 0, 0, 0, 0, 1},
    {0, 0, 0, 1, 0, 0},
    {0, 0, 0, 1, 1, 1},
    {0, 0, 0, 1, -1, 1},
    {0, 0, 0, 1, 0, 0},
    {0, 0, 0, 1, -1, 1},
    {0, 0, 0, 1, 1, 1},
    {1, 0, 0, 0, 0, 0},
    {1, 0, 1, 0, 0, 1},
    {1, 0, -1, 0, 0, 1},
    {1, 1, 0, 1, 0, 0},
    {1, 1, 1, 1, 1, 1},
    {1, 1, -1, 1, -1, 1},
    {1, -1, 0, 1, 0, 0},
    {1, -1, 1, 1, -1, 1},
    {1, -1, -1, 1, 1, 1},
    {1, 0, 0, 0, 0, 0},
    {1, 0, -1, 0, 0, 1},
    {1, 0, 1, 0, 0, 1},
    {1, -1, 0, 1, 0, 0},
    {1, -1, -1, 1, 1, 1},
    {1, -1, 1, 1, -1, 1},
    {1, 1, 0, 1, 0, 0},
    {1, 1, -1, 1, -1, 1},
    {1, 1, 1, 1, 1, 1}};

#ifdef BGK
NEON_CUDA_DEVICE_ONLY static constexpr char latticeVelocity[19][3] = {
    {-1, 0, 0} /*!  0  Symmetry first section (GO) */,
    {0, -1, 0} /*!  1  */,
    {0, 0, -1} /*!  2  */,
    {-1, -1, 0} /*! 3  */,
    {-1, 1, 0} /*!  4  */,
    {-1, 0, -1} /*! 5  */,
    {-1, 0, 1} /*!  6  */,
    {0, -1, -1} /*! 7  */,
    {0, -1, 1} /*!  8  */,
    {0, 0, 0} /*!   9  The center */,
    {1, 0, 0} /*!   10 Symmetry mirror section (BK) */,
    {0, 1, 0} /*!   11 */,
    {0, 0, 1} /*!   12 */,
    {1, 1, 0} /*!   13 */,
    {1, -1, 0} /*!  14 */,
    {1, 0, 1} /*!   15 */,
    {1, 0, -1} /*!  16 */,
    {0, 1, 1} /*!   17 */,
    {0, 1, -1} /*!  18 */
};

NEON_CUDA_DEVICE_ONLY static constexpr char latticeOppositeID[19] = {
    10 /*!  0 */,
    11 /*! 1  */,
    12 /*! 2  */,
    13 /*! 3  */,
    14 /*! 4  */,
    15 /*! 5  */,
    16 /*! 6  */,
    17 /*! 7  */,
    18 /*! 8  */,
    9 /*!  9 */,
    0 /*!  10 */,
    1 /*!  11 */,
    2 /*!  12 */,
    3 /*!  13 */,
    4 /*!  14 */,
    5 /*!  15 */,
    6 /*!  16 */,
    7 /*!  17 */,
    8 /*!  18 */
};

NEON_CUDA_DEVICE_ONLY static constexpr double latticeWeights[19] = {
    1. / 18. /*!  0   */,
    1. / 18. /*!  1   */,
    1. / 18. /*!  2   */,
    1. / 36. /*!  3   */,
    1. / 36. /*!  4   */,
    1. / 36. /*!  5   */,
    1. / 36. /*!  6   */,
    1. / 36. /*!  7   */,
    1. / 36. /*!  8   */,
    1. / 3. /*!   9  */,
    1. / 18. /*!  10   */,
    1. / 18. /*!  11  */,
    1. / 18. /*!  12  */,
    1. / 36. /*!  13  */,
    1. / 36. /*!  14  */,
    1. / 36. /*!  15  */,
    1. / 36. /*!  16  */,
    1. / 36. /*!  17  */,
    1. / 36. /*!  18  */
};
#endif