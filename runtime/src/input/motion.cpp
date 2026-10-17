// GamePad motion (gyro / accelerometer / attitude) from a host controller's sensors, e.g. the
// DualSense. Port of Cemu's input/motion (MahonySensorFusion, WiiUMotionHandler, MotionSample) and
// its SDL sensor handling (SDLControllerProvider), MPL-2.0, so values match what games expect there.
#include <cmath>
#include <mutex>

#include "input.h"

namespace input {
namespace {
constexpr float kPi = 3.14159265f;

struct Quat {
    float w = 1, x = 0, y = 0, z = 0;
    Quat operator*(const Quat& r) const {
        return {w * r.w - x * r.x - y * r.y - z * r.z, w * r.x + x * r.w + y * r.z - z * r.y,
                w * r.y - x * r.z + y * r.w + z * r.x, w * r.z + x * r.y - y * r.x + z * r.w};
    }
    void normalize_xyzw() {
        float s = 1.0f / sqrtf(w * w + x * x + y * y + z * z);
        w *= s, x *= s, y *= s, z *= s;
    }
    static Quat angle_axis(float angle, float ax, float ay, float az) {  // keeps w, scales xyz
        Quat q{cosf(angle * 0.5f), ax, ay, az};
        float s = sqrtf(1.0f - q.w * q.w) / sqrtf(ax * ax + ay * ay + az * az);
        q.x *= s, q.y *= s, q.z *= s;
        return q;
    }
};

// Mahony IMU filter with gyro bias estimation
struct Mahony {
    Quat q{sqrtf(0.5f), sqrtf(0.5f), 0, 0};  // default pose: held in hands, tilted forward
    float roll = 0, pitch = 0, yaw = 0;
    int roll_w = 0, pitch_w = 0, yaw_w = 0;
    float bias[3]{};
    double bias_sum[3]{};
    uint64_t bias_n = 0;

    void update(float dt, float gx, float gy, float gz, float ax, float ay, float az) {
        if (dt > 0.2f) dt = 0.2f;
        if (fabsf(gx) < 0.35f && fabsf(gy) < 0.35f && fabsf(gz) < 0.35f) {  // at rest: learn the bias
            bias_sum[0] += gx, bias_sum[1] += gy, bias_sum[2] += gz;
            if (++bias_n >= 200)
                for (int k = 0; k < 3; k++) bias[k] = (float)(bias_sum[k] / (double)bias_n);
        }
        float g[3] = {gx - bias[0], gy - bias[1], gz - bias[2]};
        for (float& v : g)
            if (fabsf(v) < 0.015f) v = 0;
        if (fabsf(ax) > 1e-6f || fabsf(ay) > 1e-6f || fabsf(az) > 1e-6f) {
            float len = sqrtf(ax * ax + ay * ay + az * az);
            float a[3] = {ax / len, ay / len, az / len};
            float grav[3] = {(q.x * q.z - q.w * q.y), (q.y * q.z + q.w * q.x), (q.w * q.w + q.z * q.z) - 0.5f};  // Z * 0.5
            g[0] -= grav[1] * a[2] - grav[2] * a[1];
            g[1] -= grav[2] * a[0] - grav[0] * a[2];
            g[2] -= grav[0] * a[1] - grav[1] * a[0];
        }
        for (float& v : g) v *= 0.5f * dt;
        Quat d = q * Quat{0, g[0], g[1], g[2]};
        q.w += d.w, q.x += d.x, q.y += d.y, q.z += d.z;
        q.normalize_xyzw();
        float pr = roll, pp = pitch, py = yaw;
        roll = atan2f(2 * (q.z * q.w + q.x * q.y), 1 - 2 * (q.w * q.w + q.x * q.x));
        float sp = 2 * (q.z * q.x - q.y * q.w);
        pitch = fabsf(sp) >= 1 ? copysignf(kPi / 2, sp) : asinf(sp);
        yaw = atan2f(2 * (q.z * q.y + q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y));
        auto winding = [](float prev, float now) { return now - prev > kPi ? -1 : prev - now > kPi ? 1 : 0; };
        roll_w += winding(pr, roll), pitch_w += winding(pp, pitch), yaw_w += winding(py, yaw);
    }
};

float rad_to_rev(float r) { return r / (2.0f * kPi); }

// x axis of a rotation in VPAD's mixed-handedness convention (MotionSample::getXVector)
void x_vector(const Quat& q, float* v) {
    v[0] = 1 - 2 * (q.y * q.y + q.z * q.z);
    v[2] = 2 * (q.x * q.y + q.z * q.w);
    v[1] = 2 * (q.x * q.z - q.y * q.w);
}

std::mutex g_lock;
Mahony g_imu;
bool g_valid = false, g_has_acc = false, g_has_gyro = false;
uint64_t g_ts_acc = 0, g_ts_gyro = 0, g_ts_integrate = 0;
float g_acc[3]{}, g_prev_acc[3]{}, g_gyro[3]{};  // handler space
float g_sens_acc[3]{}, g_sens_gyro[3]{};       // latest tracked samples
Motion g_out;

void integrate(float dt) {
    // WiiUMotionHandler::processMotionSample(dt, gyro, acc.x, -acc.y, -acc.z)
    float ax = g_sens_acc[0], ay = -g_sens_acc[1], az = -g_sens_acc[2];
    for (int k = 0; k < 3; k++) g_prev_acc[k] = g_acc[k], g_gyro[k] = g_sens_gyro[k];
    g_acc[0] = ax, g_acc[1] = ay, g_acc[2] = az;
    g_imu.update(dt, g_gyro[0], g_gyro[1], g_gyro[2], ax, ay, az);

    Motion& m = g_out;
    m.acc[0] = -g_acc[0], m.acc[1] = -g_acc[1], m.acc[2] = g_acc[2];
    m.acc_magnitude = sqrtf(ax * ax + ay * ay + az * az);
    float dx = g_acc[0] - g_prev_acc[0], dy = g_acc[1] - g_prev_acc[1], dz = g_acc[2] - g_prev_acc[2];
    m.acc_acceleration = sqrtf(dx * dx + dy * dy + dz * dz);
    m.gyro_change[0] = rad_to_rev(-(g_gyro[0] - g_imu.bias[0]));
    m.gyro_change[1] = rad_to_rev(-(g_gyro[1] - g_imu.bias[1]));
    m.gyro_change[2] = rad_to_rev(g_gyro[2] - g_imu.bias[2]);
    m.orientation[0] = rad_to_rev(-(g_imu.yaw + g_imu.yaw_w * 2 * kPi)) - 0.5f;
    m.orientation[1] = rad_to_rev(-(g_imu.pitch + g_imu.pitch_w * 2 * kPi)) - 0.5f;
    m.orientation[2] = rad_to_rev(g_imu.roll + g_imu.roll_w * 2 * kPi);
    x_vector(g_imu.q, m.dir + 0);
    x_vector(g_imu.q * Quat::angle_axis(kPi / 2, 0, 0, 1), m.dir + 3);
    x_vector(g_imu.q * Quat::angle_axis(kPi / 2, 0, 1, 0), m.dir + 6);
    g_valid = true;
}
}  // namespace

void motion_sensor(bool gyro, const float data[3], uint64_t ts) {
    std::lock_guard<std::mutex> l(g_lock);
    uint64_t& last = gyro ? g_ts_gyro : g_ts_acc;
    if (ts <= last) return;
    if (ts - last >= 10000000000ull) {  // stale stream: restart pairing
        g_has_acc = g_has_gyro = false;
        last = ts;
        return;
    }
    last = ts;
    if (gyro) {
        g_sens_gyro[0] = data[0], g_sens_gyro[1] = -data[1], g_sens_gyro[2] = -data[2];  // rad/s
        g_has_gyro = true;
    } else {
        for (int k = 0; k < 3; k++) g_sens_acc[k] = -data[k] / 9.81f;  // m/s^2 -> g
        g_has_acc = true;
    }
    if (g_has_acc && g_has_gyro) {
        uint64_t t = g_ts_gyro > g_ts_acc ? g_ts_gyro : g_ts_acc;
        if (t > g_ts_integrate) {
            float dt = std::fmin((float)(t - g_ts_integrate) / 1e9f, 1.0f);
            g_ts_integrate = t;
            integrate(dt);
        }
        g_has_acc = g_has_gyro = false;
    }
}

void motion_reset() {
    std::lock_guard<std::mutex> l(g_lock);
    g_imu = Mahony{};
    g_valid = g_has_acc = g_has_gyro = false;
    g_ts_acc = g_ts_gyro = g_ts_integrate = 0;
}

bool motion(Motion* out) {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_valid) return false;
    *out = g_out;
    return true;
}
}  // namespace input
