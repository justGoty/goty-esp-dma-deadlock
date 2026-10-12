#include "GUI/Fuser/ESP/Draw/PlayerBoxGeometry.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    int checks{};
    void Check(bool value, const char* message)
    {
        ++checks;
        if (!value) throw std::runtime_error(message);
    }
    bool Near(float actual, float expected) { return std::abs(actual - expected) < .01f; }
}

int main()
{
    try
    {
        // An orthographic camera with 100 world units from center to top. This
        // makes the expected bounds independent, hand-calculable fixture facts.
        Matrix44 ortho{};
        ortho.m00 = .001f; ortho.m12 = .01f; ortho.m33 = 1.f;
        const Vector3 feet{};
        const auto box = PlayerBoxGeometry::Project(ortho, feet, 1000.f, 500.f, 72.f, .45f);
        Check(box.has_value(), "upright character creates a box without bones");
        Check(Near(box->left, 460.f) && Near(box->right, 541.f), "configured width gives centered 81 pixel bounds");
        Check(Near(box->top, 70.5f) && Near(box->bottom, 250.5f), "72 world unit height gives 180 pixel bounds");
        const auto taller = PlayerBoxGeometry::Project(ortho, feet, 1000.f, 500.f, 100.f, .5f);
        Check(taller && Near(taller->top, .5f) && Near(taller->bottom, 250.5f), "height setting keeps feet anchored");
        Check(taller && Near(taller->right - taller->left, 125.f), "width setting follows projected height");
        const auto moved = PlayerBoxGeometry::Project(ortho, Vector3{100.f, 0.f, 20.f}, 1000.f, 500.f, 72.f, .45f);
        Check(moved && Near(moved->left, 510.f) && Near(moved->bottom, 200.5f), "position moves box without changing upright dimensions");
        const auto doubled = PlayerBoxGeometry::Project(ortho, feet, 2000.f, 1000.f, 72.f, .45f);
        Check(doubled && Near(doubled->right - doubled->left, 162.f) && Near(doubled->bottom - doubled->top, 360.f), "box follows actual render resolution");

        Matrix44 perspective{};
        perspective.m00 = 1.f; perspective.m12 = 1.f; perspective.m31 = 1.f;
        const auto near = PlayerBoxGeometry::Project(perspective, Vector3{0.f, 100.f, 0.f}, 1000.f, 500.f, 72.f, .45f);
        const auto far = PlayerBoxGeometry::Project(perspective, Vector3{0.f, 200.f, 0.f}, 1000.f, 500.f, 72.f, .45f);
        Check(near && far && Near(near->bottom - near->top, 180.f) && Near(far->bottom - far->top, 90.f), "perspective halves box size when distance doubles");
        Check(!PlayerBoxGeometry::Project(perspective, Vector3{0.f, -100.f, 0.f}, 1000.f, 500.f, 72.f, .45f), "characters behind camera do not draw");
        Check(!PlayerBoxGeometry::Project(perspective, Vector3{0.f, 0.f, 0.f}, 1000.f, 500.f, 72.f, .45f), "zero depth does not divide");
        Check(!PlayerBoxGeometry::Project(perspective, Vector3{0.f, .005f, 0.f}, 1000.f, 500.f, 72.f, .45f), "near plane projection is rejected");
        Check(!PlayerBoxGeometry::Project(perspective, Vector3{0.f, .01f, 0.f}, 1000.f, 500.f, 72.f, .45f), "enormous near plane bounds do not fill overlay");
        perspective.m32 = -.03f;
        Check(!PlayerBoxGeometry::Project(perspective, Vector3{0.f, 1.f, 0.f}, 1000.f, 500.f, 72.f, .45f), "head and feet straddling camera plane are rejected");

        Check(!PlayerBoxGeometry::Project(ortho, Vector3{-1200.f, 0.f, 0.f}, 1000.f, 500.f, 72.f, .45f), "wholly left of screen is rejected");
        Check(!PlayerBoxGeometry::Project(ortho, Vector3{1200.f, 0.f, 0.f}, 1000.f, 500.f, 72.f, .45f), "wholly right of screen is rejected");
        Check(PlayerBoxGeometry::Project(ortho, Vector3{1020.f, 0.f, 0.f}, 1000.f, 500.f, 72.f, .45f).has_value(), "partly visible box survives viewport clipping");
        Check(!PlayerBoxGeometry::Project(ortho, Vector3{0.f, 0.f, 400.f}, 1000.f, 500.f, 72.f, .45f), "wholly above screen is rejected");
        Check(!PlayerBoxGeometry::Project(ortho, Vector3{0.f, 0.f, -400.f}, 1000.f, 500.f, 72.f, .45f), "wholly below screen is rejected");
        Check(!PlayerBoxGeometry::Project(ortho, feet, 1000.f, 500.f, .1f, .45f), "subpixel character bounds are rejected");
        Check(!PlayerBoxGeometry::Project(ortho, feet, 1000.f, 500.f, 500.f, .45f), "absurd projected height is rejected");

        const auto nan = std::numeric_limits<float>::quiet_NaN();
        const auto inf = std::numeric_limits<float>::infinity();
        Check(!PlayerBoxGeometry::Project(ortho, Vector3{nan, 0.f, 0.f}, 1000.f, 500.f, 72.f, .45f), "NaN position never reaches drawing");
        Check(!PlayerBoxGeometry::Project(ortho, Vector3{0.f, 0.f, inf}, 1000.f, 500.f, 72.f, .45f), "infinite position never reaches drawing");
        Check(!PlayerBoxGeometry::Project(ortho, feet, nan, 500.f, 72.f, .45f), "NaN screen size is rejected");
        Check(!PlayerBoxGeometry::Project(ortho, feet, 1000.f, inf, 72.f, .45f), "infinite screen size is rejected");
        Check(!PlayerBoxGeometry::Project(ortho, feet, 0.f, 500.f, 72.f, .45f), "zero screen width is rejected");
        Check(!PlayerBoxGeometry::Project(ortho, feet, 1000.f, -1.f, 72.f, .45f), "negative screen height is rejected");
        Check(!PlayerBoxGeometry::Project(ortho, feet, 1000.f, 500.f, nan, .45f), "NaN character height is rejected");
        Check(!PlayerBoxGeometry::Project(ortho, feet, 1000.f, 500.f, 0.f, .45f), "zero character height is rejected");
        Check(!PlayerBoxGeometry::Project(ortho, feet, 1000.f, 500.f, 72.f, inf), "infinite width ratio is rejected");
        Check(!PlayerBoxGeometry::Project(ortho, feet, 1000.f, 500.f, 72.f, -1.f), "negative width ratio is rejected");
        ortho.m12 = nan;
        Check(!PlayerBoxGeometry::Project(ortho, feet, 1000.f, 500.f, 72.f, .45f), "bad matrix is rejected");
        ortho.m12 = -.01f;
        Check(!PlayerBoxGeometry::Project(ortho, feet, 1000.f, 500.f, 72.f, .45f), "inverted vertical camera does not draw inverted box");

        std::cout << "PASS: " << checks << " box geometry checks\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
