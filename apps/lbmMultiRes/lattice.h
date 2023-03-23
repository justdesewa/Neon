#include "Neon/core/types/Macros.h"

enum CellType : int
{
    bounceBack = 0,
    movingWall = 1,
    bulk = 2,
    undefined = 3,
};


/*NEON_CUDA_DEVICE_ONLY static constexpr char latticeVelocity[27][3] = {
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

NEON_CUDA_DEVICE_ONLY static constexpr float latticeWeights[27] = {
    8.0 / 27.0,
    2.0f / 27.0f,
    2.0f / 27.0f,
    2.0f / 27.0f,
    1.0f / 54.0f,
    1.0f / 54.0f,
    2.0f / 27.0f,
    1.0f / 54.0f,
    1.0f / 54.0f,
    2.0f / 27.0f,
    1.0f / 54.0f,
    1.0f / 54.0f,
    1.0f / 54.0f,
    1.0f / 216.0f,
    1.0f / 216.0f,
    1.0f / 54.0f,
    1.0f / 216.0f,
    1.0f / 216.0f,
    2.0f / 27.0f,
    1.0f / 54.0f,
    1.0f / 54.0f,
    1.0f / 54.0f,
    1.0f / 216.0f,
    1.0f / 216.0f,
    1.0f / 54.0f,
    1.0f / 216.0f,
    1.0f / 216.0f

};*/

NEON_CUDA_DEVICE_ONLY static constexpr char latticeVelocity[19][3] = {
    {0, 0, 0},
    {0, -1, 0},
    {0, 1, 0},
    {-1, 0, 0},
    {-1, -1, 0},
    {-1, 1, 0},
    {1, 0, 0},
    {1, -1, 0},
    {1, 1, 0},

    {0, 0, -1},
    {0, -1, -1},
    {0, 1, -1},
    {-1, 0, -1},
    {1, 0, -1},

    {0, 0, 1},
    {0, -1, 1},
    {0, 1, 1},
    {-1, 0, 1},
    {1, 0, 1},

};

NEON_CUDA_DEVICE_ONLY static constexpr char latticeOppositeID[19] = {
    0, 2, 1, 6, 8, 7, 3, 5, 4, 14, 16, 15, 18, 17, 9, 11, 10, 13, 12};

NEON_CUDA_DEVICE_ONLY static constexpr float latticeWeights[19] = {
    1.0f / 3.0f,
    2.0f / 36.0f,
    2.0f / 36.0f,
    2.0f / 36.0f,
    1.0f / 36.0f,
    1.0f / 36.0f,
    2.0f / 36.0f,
    1.0f / 36.0f,
    1.0f / 36.0f,
    2.0f / 36.0f,
    1.0f / 36.0f,
    1.0f / 36.0f,
    1.0f / 36.0f,
    1.0f / 36.0f,
    2.0f / 36.0f,
    1.0f / 36.0f,
    1.0f / 36.0f,
    1.0f / 36.0f,
    1.0f / 36.0f
};
