#pragma once
#include <cstdint>

// Disc and track position of each bgm.dat id on the official soundtrack
// (ELJM-30777, 2 discs, 38 tracks). Matched by the Japanese title in bgm.dat;
// the album uses the same titles, in a different order.
namespace ost {

struct TrackPosition {
    uint64_t bgmId;
    int disc;
    int track;
};

constexpr TrackPosition kTracklist[] = {
    {1, 1, 1},   {2, 1, 2},   {3, 1, 3},   {4, 1, 4},   {35, 1, 5},  {5, 1, 6},
    {6, 1, 7},   {7, 1, 8},   {28, 1, 9},  {8, 1, 10},  {9, 1, 11},  {10, 1, 12},
    {27, 1, 13}, {11, 1, 14}, {12, 1, 15}, {13, 1, 16}, {26, 1, 17}, {29, 1, 18},
    {36, 2, 1},  {37, 2, 2},  {38, 2, 3},  {14, 2, 4},  {15, 2, 5},  {16, 2, 6},
    {30, 2, 7},  {17, 2, 8},  {18, 2, 9},  {19, 2, 10}, {31, 2, 11}, {20, 2, 12},
    {21, 2, 13}, {22, 2, 14}, {32, 2, 15}, {23, 2, 16}, {24, 2, 17}, {25, 2, 18},
    {33, 2, 19}, {34, 2, 20},
};

inline const TrackPosition* FindTrack(uint64_t bgmId) {
    for (const TrackPosition& position : kTracklist) {
        if (position.bgmId == bgmId) {
            return &position;
        }
    }
    return nullptr;
}

}  // namespace ost
