#pragma once

#include <cmath>
#include <cstddef>
#include <type_traits>

namespace velaros_navigation {

template <typename T>
struct Vector2 {
    T values[2]{};

    constexpr Vector2() = default;
    constexpr Vector2(T x_value, T y_value)
        : values{x_value, y_value} {}

    template <typename U>
    constexpr Vector2(const Vector2<U>& other)
        : values{static_cast<T>(other.x()), static_cast<T>(other.y())} {}

    static constexpr Vector2 Zero() { return Vector2{}; }

    T& x() { return values[0]; }
    const T& x() const { return values[0]; }
    T& y() { return values[1]; }
    const T& y() const { return values[1]; }

    void setZero() {
        values[0] = T{};
        values[1] = T{};
    }

    bool allFinite() const {
        return std::isfinite(static_cast<double>(values[0])) &&
               std::isfinite(static_cast<double>(values[1]));
    }

    T squaredNorm() const { return x() * x() + y() * y(); }

    T dot(const Vector2& other) const {
        return x() * other.x() + y() * other.y();
    }

    double norm() const {
        return std::sqrt(static_cast<double>(squaredNorm()));
    }

    Vector2& operator+=(const Vector2& other) {
        x() += other.x();
        y() += other.y();
        return *this;
    }

    Vector2& operator-=(const Vector2& other) {
        x() -= other.x();
        y() -= other.y();
        return *this;
    }

    Vector2& operator*=(T scalar) {
        x() *= scalar;
        y() *= scalar;
        return *this;
    }

    Vector2& operator/=(T scalar) {
        x() /= scalar;
        y() /= scalar;
        return *this;
    }
};

template <typename L, typename R>
inline Vector2<std::common_type_t<L, R>> operator+(
    const Vector2<L>& lhs, const Vector2<R>& rhs) {
    using Common = std::common_type_t<L, R>;
    return Vector2<Common>(static_cast<Common>(lhs.x()) + rhs.x(),
                           static_cast<Common>(lhs.y()) + rhs.y());
}

template <typename L, typename R>
inline Vector2<std::common_type_t<L, R>> operator-(
    const Vector2<L>& lhs, const Vector2<R>& rhs) {
    using Common = std::common_type_t<L, R>;
    return Vector2<Common>(static_cast<Common>(lhs.x()) - rhs.x(),
                           static_cast<Common>(lhs.y()) - rhs.y());
}

template <typename T>
inline Vector2<T> operator-(const Vector2<T>& value) {
    return Vector2<T>(-value.x(), -value.y());
}

template <typename T>
inline Vector2<T> operator*(Vector2<T> value, T scalar) {
    value *= scalar;
    return value;
}

template <typename T>
inline Vector2<T> operator*(T scalar, Vector2<T> value) {
    value *= scalar;
    return value;
}

template <typename T>
inline Vector2<T> operator/(Vector2<T> value, T scalar) {
    value /= scalar;
    return value;
}

template <typename T>
struct Vector3 {
    T values[3]{};

    constexpr Vector3() = default;
    constexpr Vector3(T x_value, T y_value, T z_value)
        : values{x_value, y_value, z_value} {}

    static constexpr Vector3 Zero() { return Vector3{}; }

    T& x() { return values[0]; }
    const T& x() const { return values[0]; }
    T& y() { return values[1]; }
    const T& y() const { return values[1]; }
    T& z() { return values[2]; }
    const T& z() const { return values[2]; }

    bool allFinite() const {
        return std::isfinite(static_cast<double>(x())) &&
               std::isfinite(static_cast<double>(y())) &&
               std::isfinite(static_cast<double>(z()));
    }
};

struct Matrix3d {
    double values[3][3]{};

    static Matrix3d Identity() {
        Matrix3d result;
        result.values[0][0] = 1.0;
        result.values[1][1] = 1.0;
        result.values[2][2] = 1.0;
        return result;
    }

    double& operator()(std::size_t row, std::size_t column) {
        return values[row][column];
    }

    const double& operator()(std::size_t row, std::size_t column) const {
        return values[row][column];
    }

    bool allFinite() const {
        for (const auto& row : values) {
            for (double value : row) {
                if (!std::isfinite(value)) {
                    return false;
                }
            }
        }
        return true;
    }
};

using Vector2f = Vector2<float>;
using Vector2d = Vector2<double>;
using Vector3f = Vector3<float>;
using Vector3d = Vector3<double>;

inline Matrix3d RotationFromYaw(double yaw) {
    Matrix3d result = Matrix3d::Identity();
    const double cosine = std::cos(yaw);
    const double sine = std::sin(yaw);
    result(0, 0) = cosine;
    result(0, 1) = -sine;
    result(1, 0) = sine;
    result(1, 1) = cosine;
    return result;
}

}  // namespace velaros_navigation
