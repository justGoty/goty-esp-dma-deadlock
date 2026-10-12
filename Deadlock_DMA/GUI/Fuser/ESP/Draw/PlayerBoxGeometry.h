#pragma once
#include "Deadlock/Engine/Matrix.h"
#include "Deadlock/Engine/Vector3.h"
#include "Deadlock/Engine/Vector2.h"
#include <cmath>
#include <optional>

namespace PlayerBoxGeometry
{
    struct Rect { float left, top, right, bottom; };

    // Approximate upright character bounds, independent of remote bone/model data.
    // Both points use the same camera snapshot so a camera update cannot shear a box.
    inline std::optional<Rect> Project(const Matrix44& matrix, const Vector3& feet,
        float screenWidth, float screenHeight, float worldHeight, float widthRatio)
    {
        if (!std::isfinite(screenWidth) || !std::isfinite(screenHeight) ||
            !std::isfinite(worldHeight) || !std::isfinite(widthRatio) ||
            screenWidth <= 0.f || screenHeight <= 0.f || worldHeight <= 0.f || widthRatio <= 0.f)
            return std::nullopt;

        const auto projectPoint = [&](const Vector3& world) -> std::optional<Vector2>
        {
            const auto x = matrix.m00 * world.x + matrix.m01 * world.y + matrix.m02 * world.z + matrix.m03;
            const auto y = matrix.m10 * world.x + matrix.m11 * world.y + matrix.m12 * world.z + matrix.m13;
            const auto w = matrix.m30 * world.x + matrix.m31 * world.y + matrix.m32 * world.z + matrix.m33;
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || w < 0.01f)
                return std::nullopt;
            const Vector2 point{screenWidth * .5f + .5f * (x / w) * screenWidth + .5f,
                screenHeight * .5f - .5f * (y / w) * screenHeight + .5f};
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) return std::nullopt;
            return point;
        };

        const auto bottom = projectPoint(feet);
        const auto top = projectPoint(Vector3{feet.x, feet.y, feet.z + worldHeight});
        if (!bottom || !top) return std::nullopt;
        const auto height = bottom->y - top->y;
        const auto width = height * widthRatio;
        if (!std::isfinite(height) || !std::isfinite(width) || height < 2.f || width < 1.f ||
            height > screenHeight * 2.f || width > screenWidth * 2.f)
            return std::nullopt;

        const auto centerX = (top->x + bottom->x) * .5f;
        const Rect box{centerX - width * .5f, top->y, centerX + width * .5f, bottom->y};
        if (!std::isfinite(box.left) || !std::isfinite(box.right) || box.right <= 0.f ||
            box.left >= screenWidth || box.bottom <= 0.f || box.top >= screenHeight)
            return std::nullopt;
        return box;
    }
}
