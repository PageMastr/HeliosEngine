// Colour conversions and packing (see helios/math/color.h).
#include "fp_control.h"

#include "helios/math/color.h"
#include "helios/math/pack.h"

#include <cmath>

namespace helios {
namespace {

// srgbToLinear(i / 255) for every 8-bit code, evaluated in 50-digit arithmetic and rounded once
// to f32 (generated offline; bit-identical on every platform).
constexpr f32 kSrgb8ToLinear[256] = {
    0.0f,           0.000303526991f, 0.000607053982f, 0.000910580973f, 0.00121410796f,
    0.00151763496f, 0.00182116195f,  0.00212468882f,  0.00242821593f,  0.0027317428f,
    0.00303526991f, 0.00334653584f,  0.00367650739f,  0.00402471703f,  0.00439144205f,
    0.00477695325f, 0.00518151652f,  0.00560539169f,  0.00604883302f,  0.00651209056f,
    0.00699541019f, 0.00749903219f,  0.00802319311f,  0.00856812578f,  0.00913405884f,
    0.00972121768f, 0.010329823f,    0.0109600937f,   0.0116122449f,   0.012286488f,
    0.0129830325f,  0.0137020834f,   0.0144438436f,   0.0152085144f,   0.0159962941f,
    0.0168073755f,  0.0176419541f,   0.01850022f,     0.0193823613f,   0.0202885624f,
    0.0212190095f,  0.0221738853f,   0.0231533665f,   0.0241576321f,   0.0251868591f,
    0.0262412224f,  0.0273208916f,   0.02842604f,     0.0295568351f,   0.0307134446f,
    0.0318960324f,  0.0331047662f,   0.0343398079f,   0.0356013142f,   0.0368894488f,
    0.0382043719f,  0.0395462364f,   0.0409151986f,   0.0423114114f,   0.043735031f,
    0.045186203f,   0.0466650873f,   0.0481718257f,   0.0497065671f,   0.0512694567f,
    0.0528606474f,  0.054480277f,    0.0561284907f,   0.0578054301f,   0.0595112368f,
    0.0612460524f,  0.0630100146f,   0.064803265f,    0.0666259378f,   0.0684781671f,
    0.0703600943f,  0.0722718537f,   0.0742135718f,   0.0761853829f,   0.078187421f,
    0.0802198201f,  0.0822827071f,   0.0843762085f,   0.0865004584f,   0.0886555836f,
    0.0908417106f,  0.0930589661f,   0.0953074694f,   0.097587347f,    0.0998987257f,
    0.102241732f,   0.104616486f,    0.107023105f,    0.10946171f,     0.111932427f,
    0.114435375f,   0.116970666f,    0.119538426f,    0.122138776f,    0.124771819f,
    0.127437681f,   0.130136475f,    0.13286832f,     0.135633335f,    0.138431609f,
    0.141263291f,   0.144128472f,    0.147027269f,    0.149959788f,    0.152926147f,
    0.155926466f,   0.158960834f,    0.162029371f,    0.165132195f,    0.168269396f,
    0.171441108f,   0.174647406f,    0.177888423f,    0.18116425f,     0.18447499f,
    0.187820777f,   0.191201687f,    0.194617838f,    0.198069319f,    0.20155625f,
    0.205078736f,   0.208636865f,    0.212230757f,    0.215860501f,    0.219526201f,
    0.223227963f,   0.226965874f,    0.230740055f,    0.23455058f,     0.238397568f,
    0.242281124f,   0.246201321f,    0.25015828f,     0.254152089f,    0.258182853f,
    0.262250662f,   0.266355604f,    0.270497799f,    0.274677306f,    0.278894275f,
    0.283148736f,   0.287440836f,    0.291770637f,    0.296138257f,    0.300543785f,
    0.304987311f,   0.309468925f,    0.313988715f,    0.318546772f,    0.323143214f,
    0.327778101f,   0.332451522f,    0.337163627f,    0.341914415f,    0.346704066f,
    0.351532608f,   0.356400132f,    0.361306787f,    0.366252601f,    0.371237695f,
    0.376262128f,   0.38132602f,     0.386429429f,    0.391572475f,    0.396755219f,
    0.401977777f,   0.407240212f,    0.412542611f,    0.417885065f,    0.423267663f,
    0.428690493f,   0.434153646f,    0.439657182f,    0.445201188f,    0.450785786f,
    0.456411034f,   0.462076992f,    0.467783809f,    0.473531485f,    0.479320168f,
    0.48514995f,    0.491020858f,    0.496932983f,    0.502886474f,    0.50888133f,
    0.514917672f,   0.520995557f,    0.527115107f,    0.533276379f,    0.539479494f,
    0.545724452f,   0.55201143f,     0.558340371f,    0.564711511f,    0.571124852f,
    0.577580452f,   0.584078431f,    0.590618849f,    0.597201765f,    0.603827357f,
    0.610495567f,   0.617206573f,    0.623960376f,    0.630757153f,    0.637596846f,
    0.644479692f,   0.651405632f,    0.658374846f,    0.665387273f,    0.672443151f,
    0.679542482f,   0.686685324f,    0.693871737f,    0.701101899f,    0.708375752f,
    0.715693474f,   0.723055124f,    0.730460763f,    0.73791039f,     0.745404184f,
    0.752942204f,   0.760524511f,    0.768151164f,    0.775822222f,    0.783537805f,
    0.791297913f,   0.799102724f,    0.806952238f,    0.814846575f,    0.822785735f,
    0.830769897f,   0.838799f,       0.846873224f,    0.854992628f,    0.863157213f,
    0.871367097f,   0.8796224f,      0.887923121f,    0.896269381f,    0.904661179f,
    0.913098633f,   0.921581864f,    0.930110872f,    0.938685715f,    0.947306514f,
    0.955973327f,   0.964686275f,    0.973445296f,    0.982250571f,    0.991102099f,
    1.0f,
};

// Linear-space decision thresholds: srgbToLinear((i + 0.5) / 255) for i = 0..254. The nearest
// 8-bit code of a linear value x is the number of thresholds <= x (monotonic transfer function).
constexpr f32 kLinearToSrgb8Threshold[255] = {
    0.000151763496f, 0.000455290487f, 0.000758817478f, 0.00106234441f, 0.0013658714f,  0.00166939839f,
    0.00197292538f,  0.00227645249f,  0.00257997937f,  0.00288350624f, 0.00318830088f, 0.00350925932f,
    0.00384831498f,  0.00420574797f,  0.00458183279f,  0.00497683743f, 0.00539102405f, 0.00582465064f,
    0.00627796957f,  0.00675122766f,  0.00724466844f,  0.00775853032f, 0.00829304848f, 0.00884845294f,
    0.00942497049f,  0.0100228256f,   0.010642237f,    0.011283421f,   0.0119465925f,  0.0126319602f,
    0.0133397318f,   0.0140701123f,   0.0148233026f,   0.0155995032f,  0.0163989104f,  0.0172217153f,
    0.0180681143f,   0.0189382937f,   0.0198324434f,   0.0207507443f,  0.0216933824f,  0.0226605386f,
    0.0236523896f,   0.0246691145f,   0.0257108882f,   0.0267778821f,  0.0278702695f,  0.0289882198f,
    0.0301319025f,   0.0313014798f,   0.0324971229f,   0.0337189883f,  0.0349672437f,  0.0362420455f,
    0.0375435539f,   0.0388719253f,   0.04022732f,     0.041609887f,   0.0430197865f,  0.0444571637f,
    0.0459221713f,   0.0474149622f,   0.0489356853f,   0.0504844859f,  0.0520615056f,  0.0536668971f,
    0.055300802f,    0.0569633618f,   0.0586547181f,   0.0603750125f,  0.0621243827f,  0.0639029741f,
    0.0657109171f,   0.0675483495f,   0.0694154128f,   0.0713122338f,  0.0732389539f,  0.0751957074f,
    0.0771826133f,   0.0791998208f,   0.0812474415f,   0.0833256245f,  0.085434489f,   0.0875741541f,
    0.089744769f,    0.091946438f,    0.0941793025f,   0.0964434743f,  0.098739095f,   0.101066269f,
    0.10342513f,     0.105815805f,    0.108238399f,    0.110693045f,   0.113179862f,   0.115698971f,
    0.118250482f,    0.120834522f,    0.123451203f,    0.126100644f,   0.128782958f,   0.131498262f,
    0.134246677f,    0.137028307f,    0.13984327f,     0.142691687f,   0.145573661f,   0.148489311f,
    0.151438728f,    0.15442206f,     0.157439381f,    0.160490826f,   0.163576499f,   0.166696489f,
    0.169850931f,    0.173039913f,    0.176263571f,    0.179521978f,   0.182815254f,   0.186143503f,
    0.189506829f,    0.192905352f,    0.196339145f,    0.199808344f,   0.203313038f,   0.206853345f,
    0.210429341f,    0.214041144f,    0.217688844f,    0.22137256f,    0.225092396f,   0.228848428f,
    0.232640758f,    0.236469507f,    0.240334779f,    0.244236633f,   0.248175204f,   0.252150565f,
    0.256162852f,    0.260212123f,    0.264298469f,    0.268422037f,   0.272582889f,   0.276781112f,
    0.281016797f,    0.285290092f,    0.289601028f,    0.293949723f,   0.298336297f,   0.30276081f,
    0.30722335f,     0.311724037f,    0.31626296f,     0.32084018f,    0.325455844f,   0.330109984f,
    0.334802747f,    0.339534163f,    0.344304383f,    0.349113464f,   0.353961498f,   0.358848572f,
    0.363774776f,    0.368740231f,    0.373744965f,    0.378789127f,   0.383872777f,   0.388996005f,
    0.3941589f,      0.399361521f,    0.404604018f,    0.40988642f,    0.415208817f,   0.420571357f,
    0.425974041f,    0.431417018f,    0.436900347f,    0.442424119f,   0.447988421f,   0.453593314f,
    0.459238917f,    0.464925289f,    0.470652521f,    0.476420701f,   0.482229918f,   0.488080233f,
    0.493971765f,    0.499904543f,    0.505878687f,    0.511894286f,   0.517951429f,   0.524050117f,
    0.530190527f,    0.536372721f,    0.542596757f,    0.548862696f,   0.555170655f,   0.561520696f,
    0.567912877f,    0.574347317f,    0.580824137f,    0.587343335f,   0.593904972f,   0.600509226f,
    0.607156098f,    0.613845706f,    0.62057811f,     0.62735337f,    0.634171605f,   0.641032875f,
    0.647937238f,    0.654884815f,    0.661875665f,    0.668909788f,   0.675987363f,   0.683108449f,
    0.690273106f,    0.697481334f,    0.704733372f,    0.712029159f,   0.719368815f,   0.72675246f,
    0.734180033f,    0.741651773f,    0.749167681f,    0.756727815f,   0.764332294f,   0.77198112f,
    0.779674411f,    0.787412286f,    0.795194745f,    0.803021908f,   0.810893834f,   0.818810523f,
    0.826772213f,    0.834778786f,    0.842830479f,    0.850927293f,   0.859069228f,   0.867256522f,
    0.875489056f,    0.883767068f,    0.892090559f,    0.900459588f,   0.908874214f,   0.917334557f,
    0.925840616f,    0.934392571f,    0.942990363f,    0.951634169f,   0.960324049f,   0.969060004f,
    0.977842152f,    0.986670554f,    0.995545268f,
};

f32 clamp01(f32 x) noexcept { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; }  // NaN -> 0
f32 clampNonNegative(f32 x, f32 hi) noexcept { return x > 0.0f ? (x < hi ? x : hi) : 0.0f; }

}  // namespace

f32 srgb8ToLinear(u8 v) noexcept { return kSrgb8ToLinear[v]; }

u8 linearToSrgb8(f32 linear) noexcept {
    const f32 x = clamp01(linear);
    // Branch-light binary search over 255 sorted thresholds (8 steps).
    u32 lo = 0;
    u32 count = 255;
    while (count > 0) {
        const u32 half = count / 2;
        if (kLinearToSrgb8Threshold[lo + half] <= x) {
            lo += half + 1;
            count -= half + 1;
        } else {
            count = half;
        }
    }
    return static_cast<u8>(lo);
}

Color Color::fromSrgb8(u8 r8, u8 g8, u8 b8, u8 a8) noexcept {
    return {kSrgb8ToLinear[r8], kSrgb8ToLinear[g8], kSrgb8ToLinear[b8], unpackUnorm8(a8)};
}

Color Color::fromSrgbHex(u32 rrggbb) noexcept {
    return fromSrgb8(static_cast<u8>(rrggbb >> 16), static_cast<u8>(rrggbb >> 8), static_cast<u8>(rrggbb),
                     255);
}

u32 packRGBA8(const Color& c) noexcept { return packUnorm4x8(c.rgba()); }

Color unpackRGBA8(u32 p) noexcept { return Color(unpackUnorm4x8(p)); }

u32 packSRGBA8(const Color& c) noexcept {
    return static_cast<u32>(linearToSrgb8(c.r)) | (static_cast<u32>(linearToSrgb8(c.g)) << 8) |
           (static_cast<u32>(linearToSrgb8(c.b)) << 16) | (quantizeUnorm(c.a, 8) << 24);
}

Color unpackSRGBA8(u32 p) noexcept {
    return Color::fromSrgb8(static_cast<u8>(p), static_cast<u8>(p >> 8), static_cast<u8>(p >> 16),
                            static_cast<u8>(p >> 24));
}

u32 packRGB10A2(const Color& c) noexcept {
    return quantizeUnorm(c.r, 10) | (quantizeUnorm(c.g, 10) << 10) | (quantizeUnorm(c.b, 10) << 20) |
           (quantizeUnorm(c.a, 2) << 30);
}

Color unpackRGB10A2(u32 p) noexcept {
    return {dequantizeUnorm(p & 0x3FFu, 10), dequantizeUnorm((p >> 10) & 0x3FFu, 10),
            dequantizeUnorm((p >> 20) & 0x3FFu, 10), dequantizeUnorm(p >> 30, 2)};
}

// Shared-exponent encoding per the Vulkan / EXT_texture_shared_exponent specification.
u32 packRGB9E5(const Vec3& rgb) noexcept {
    constexpr int kMantissaBits = 9;
    constexpr int kBias = 15;
    constexpr f32 kMaxValue = 65408.0f;  // (511 / 512) * 2^16
    const f32 r = clampNonNegative(rgb.x, kMaxValue);
    const f32 g = clampNonNegative(rgb.y, kMaxValue);
    const f32 b = clampNonNegative(rgb.z, kMaxValue);
    const f32 maxc = max(max(r, g), b);
    if (!(maxc > 0.0f)) return 0u;
    int e2 = 0;
    (void)std::frexp(maxc, &e2);  // maxc = m * 2^e2 with m in [0.5, 1): floor(log2(maxc)) = e2 - 1
    int expShared = max(-kBias - 1, e2 - 1) + 1 + kBias;
    f32 denom = std::ldexp(1.0f, expShared - kBias - kMantissaBits);
    const i32 maxs = static_cast<i32>(std::floor(maxc / denom + 0.5f));
    if (maxs == (1 << kMantissaBits)) {
        ++expShared;
        denom *= 2.0f;
    }
    const u32 rs = static_cast<u32>(std::floor(r / denom + 0.5f));
    const u32 gs = static_cast<u32>(std::floor(g / denom + 0.5f));
    const u32 bs = static_cast<u32>(std::floor(b / denom + 0.5f));
    return rs | (gs << 9) | (bs << 18) | (static_cast<u32>(expShared) << 27);
}

Vec3 unpackRGB9E5(u32 p) noexcept {
    const f32 scale = std::ldexp(1.0f, static_cast<int>(p >> 27) - 15 - 9);
    return {static_cast<f32>(p & 0x1FFu) * scale, static_cast<f32>((p >> 9) & 0x1FFu) * scale,
            static_cast<f32>((p >> 18) & 0x1FFu) * scale};
}

// Radiance RGBE (G. Ward, Graphics Gems II): mantissas truncated, decoded at bucket centres.
u32 packRGBE(const Vec3& rgb) noexcept {
    constexpr f32 kMax = 1.7e38f;
    const f32 r = clampNonNegative(rgb.x, kMax);
    const f32 g = clampNonNegative(rgb.y, kMax);
    const f32 b = clampNonNegative(rgb.z, kMax);
    const f32 v = max(max(r, g), b);
    if (!(v >= 1e-32f)) return 0u;
    int e = 0;
    (void)std::frexp(v, &e);                    // v = m * 2^e, m in [0.5, 1)
    const f32 scale = std::ldexp(1.0f, 8 - e);  // maps [0, 2^e) to [0, 256)
    const u32 rm = min(static_cast<u32>(r * scale), 255u);
    const u32 gm = min(static_cast<u32>(g * scale), 255u);
    const u32 bm = min(static_cast<u32>(b * scale), 255u);
    return rm | (gm << 8) | (bm << 16) | (static_cast<u32>(e + 128) << 24);
}

Vec3 unpackRGBE(u32 p) noexcept {
    const u32 e = p >> 24;
    if (e == 0) return {};
    const f32 f = std::ldexp(1.0f, static_cast<int>(e) - (128 + 8));
    return {(static_cast<f32>(p & 0xFFu) + 0.5f) * f, (static_cast<f32>((p >> 8) & 0xFFu) + 0.5f) * f,
            (static_cast<f32>((p >> 16) & 0xFFu) + 0.5f) * f};
}

Vec4 encodeRGBM(const Vec3& rgb, f32 range) noexcept {
    const Vec3 c{clampNonNegative(rgb.x, range), clampNonNegative(rgb.y, range),
                 clampNonNegative(rgb.z, range)};
    f32 m = maxComponent(c) / range;
    m = std::ceil(m * 255.0f) / 255.0f;
    if (!(m > 0.0f)) return {0.0f, 0.0f, 0.0f, 0.0f};
    const f32 inv = 1.0f / (m * range);
    return {min(c.x * inv, 1.0f), min(c.y * inv, 1.0f), min(c.z * inv, 1.0f), m};
}

Color kelvinToRgb(f32 kelvin) noexcept {
    const f64 t =
        kelvin > 1000.0f ? (kelvin < 40000.0f ? static_cast<f64>(kelvin) : 40000.0) : 1000.0;  // NaN -> 1000
    const f64 t2 = t * t;
    // Krystek (1985): Planckian locus in CIE 1960 (u, v).
    const f64 u = (0.860117757 + 1.54118254e-4 * t + 1.28641212e-7 * t2) /
                  (1.0 + 8.42420235e-4 * t + 7.08145163e-7 * t2);
    const f64 v = (0.317398726 + 4.22806245e-5 * t + 4.20481691e-8 * t2) /
                  (1.0 - 2.89741816e-5 * t + 1.61456053e-7 * t2);
    // (u, v) -> CIE xy -> XYZ with Y = 1.
    const f64 d = 2.0 * u - 8.0 * v + 4.0;
    const f64 x = 3.0 * u / d;
    const f64 y = 2.0 * v / d;
    const f64 X = x / y;
    const f64 Z = (1.0 - x - y) / y;
    // XYZ -> linear sRGB (D65).
    const f64 rl = 3.2404542 * X - 1.5371385 - 0.4985314 * Z;
    const f64 gl = -0.9692660 * X + 1.8760108 + 0.0415560 * Z;
    const f64 bl = 0.0556434 * X - 0.2040259 + 1.0572252 * Z;
    const f64 r = rl > 0.0 ? rl : 0.0;
    const f64 g = gl > 0.0 ? gl : 0.0;
    const f64 b = bl > 0.0 ? bl : 0.0;
    const f64 m = max(max(r, g), b);
    return {static_cast<f32>(r / m), static_cast<f32>(g / m), static_cast<f32>(b / m), 1.0f};
}

}  // namespace helios
