// Implementation of Pimoroni common geometry functions

#include "pico_graphics.hpp"

namespace pimoroni {

bool Rect::empty() const {
    return w <= 0 || h <= 0;
}

bool Rect::contains(const Point &p) const {
    return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h;
}

bool Rect::contains(const Rect &r) const {
    return r.x >= x && r.y >= y && r.x + r.w <= x + w && r.y + r.h <= y + h;
}

bool Rect::intersects(const Rect &r) const {
    return !(r.x >= x + w || r.x + r.w <= x || r.y >= y + h || r.y + r.h <= y);
}

Rect Rect::intersection(const Rect &r) const {
    int32_t nx = x > r.x ? x : r.x;
    int32_t ny = y > r.y ? y : r.y;
    int32_t nw = ((x + w) < (r.x + r.w) ? (x + w) : (r.x + r.w)) - nx;
    int32_t nh = ((y + h) < (r.y + r.h) ? (y + h) : (r.y + r.h)) - ny;
    return Rect(nx, ny, nw, nh);
}

void Rect::inflate(int32_t v) {
    x -= v;
    y -= v;
    w += v * 2;
    h += v * 2;
}

void Rect::deflate(int32_t v) {
    x += v;
    y += v;
    w -= v * 2;
    h -= v * 2;
}

Point Point::clamp(const Rect &r) const {
    return Point(
        x < r.x ? r.x : (x >= r.x + r.w ? r.x + r.w - 1 : x),
        y < r.y ? r.y : (y >= r.y + r.h ? r.y + r.h - 1 : y)
    );
}

} // namespace pimoroni
