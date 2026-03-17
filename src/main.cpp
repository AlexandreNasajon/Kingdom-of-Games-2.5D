// ═══════════════════════════════════════════════════════════════════════════════
// "Sovereign Unova" — 2.5D Desert Village Engine  (Pokémon BW style)
// Raylib 5.5 · C++17 · Hybrid 3D terrain + 2D billboard sprites
// POST-PROCESSING: Bloom, God Rays, Heat Haze, DoF, Color Grading, Vignette
// ═══════════════════════════════════════════════════════════════════════════════
#include "imgui.h"
#include "input.h"
#include "raylib.h"
#include "raymath.h"
#include "rlImGui.h"
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// Forward declaration for dune height sampling used across systems
static float GetDuneHeight(float x, float z);

// ═══════════════════════════════════════════════════════════════════════════════
// GLSL 330 SHADER SOURCES
// ═══════════════════════════════════════════════════════════════════════════════
static const char *FS_BLOOM_EXTRACT = R"(
#version 330
in vec2 fragTexCoord; in vec4 fragColor;
uniform sampler2D texture0; uniform vec4 colDiffuse;
uniform float threshold;
out vec4 finalColor;
void main(){
    vec4 c=texture(texture0,fragTexCoord)*colDiffuse*fragColor;
    float br=dot(c.rgb,vec3(0.2126,0.7152,0.0722));
    finalColor=(br>threshold)?c:vec4(0.0,0.0,0.0,1.0);
})";

static const char *FS_BLUR = R"(
#version 330
in vec2 fragTexCoord; in vec4 fragColor;
uniform sampler2D texture0; uniform vec4 colDiffuse;
uniform vec2 direction;
out vec4 finalColor;
void main(){
    float w[5]=float[](0.227027,0.1945946,0.1216216,0.054054,0.016216);
    vec4 s=texture(texture0,fragTexCoord)*w[0];
    for(int i=1;i<5;i++){
        vec2 off=direction*float(i);
        s+=texture(texture0,fragTexCoord+off)*w[i];
        s+=texture(texture0,fragTexCoord-off)*w[i];
    }
    finalColor=s;
})";

static const char *FS_GODRAYS = R"(
#version 330
in vec2 fragTexCoord; in vec4 fragColor;
uniform sampler2D texture0; uniform vec4 colDiffuse;
uniform vec2 sunPos;
out vec4 finalColor;
void main(){
    vec2 uv=fragTexCoord;
    vec2 delta=(uv-sunPos)*(1.0/60.0)*0.98;
    vec4 s=vec4(0.0); float decay=1.0;
    for(int i=0;i<60;i++){
        uv-=delta;
        s+=texture(texture0,clamp(uv,0.0,1.0))*decay*0.015;
        decay*=0.975;
    }
    finalColor=vec4(s.rgb,1.0);
})";

// Final composite: heat haze + DoF + bloom + godrays + #FFF4D6 sun + color LUT
// + vibrance
static const char *FS_COMPOSITE = R"(
#version 330
in vec2 fragTexCoord; in vec4 fragColor;
uniform sampler2D texture0;
uniform sampler2D bloomTex;
uniform sampler2D godrayTex;
uniform float bloomStr;
uniform float godrayStr;
uniform float time;
uniform float exposure;
uniform vec2 resolution;
uniform vec4 colDiffuse;
out vec4 finalColor;
void main(){
    vec2 uv=fragTexCoord;
    // ── Heat haze: only distorts background desert, NOT player/UI area
    // Attenuate in the vertical focus band (0.40–0.70) where sprites live
    float haze=sin(uv.y*25.0+time*1.5)*0.0018+sin(uv.y*40.0+time*2.3)*0.0010;
    haze+=sin(uv.x*18.0+time*0.9)*0.0006;
    haze*=(1.0-uv.y)*2.0;  // stronger near ground (low uv.y = top)
    // Suppress haze in the center sprite band to preserve pixel-art clarity
    float spriteBand=1.0-smoothstep(0.08,0.18,abs(uv.y-0.55));
    haze*=(1.0-spriteBand*0.85);
    uv.x+=haze;
    // ── Depth of Field: 8-tap circular blur, strength from distance to focus band
    float focusY=0.55;
    float dofDist=abs(uv.y-focusY);
    float blurR=smoothstep(0.10,0.45,dofDist)*0.6/resolution.y;
    vec3 scene=texture(texture0,uv).rgb;
    if(blurR>0.0001){
        vec3 sum=scene;
        for(int i=0;i<8;i++){
            float a=float(i)*0.785398;
            sum+=texture(texture0,uv+vec2(cos(a),sin(a))*blurR).rgb;
        }
        scene=sum/9.0;
    }
    // ── Bloom + God rays (stable, single-pass)
    vec3 bloom=texture(bloomTex,fragTexCoord).rgb;
    vec3 rays=texture(godrayTex,fragTexCoord).rgb;
    vec3 c=scene+bloom*bloomStr+rays*godrayStr;
    // ── Sun color temperature #FFF4D6 — hard-coded constant (no oscillators)
    const vec3 SUN_TEMP=vec3(1.0,0.957,0.839);
    c*=SUN_TEMP*1.10;
    // ── Static exposure — hard-coded constant 1.1 (directive-locked, no auto-exposure)
    c*=1.10;
    // ── Stable vibrance: safe clamped saturation boost
    float lum=dot(c,vec3(0.2126,0.7152,0.0722));
    vec3 diff=c-vec3(lum);
    float satLen=length(diff);
    float boost=clamp(1.0-satLen*1.5,0.0,1.0)*0.30; // only boost desaturated
    c=vec3(lum)+diff*(1.0+boost);
    // ── Color LUT: warm desert grade (static constants, no time dependency)
    c.r=pow(max(c.r,0.0),0.88)*1.12;
    c.g=pow(max(c.g,0.0),0.94)*1.06;
    c.b=pow(max(c.b,0.0),1.16)*0.80;
    // ── Contrast S-curve
    c=(c-0.5)*1.20+0.5;
    c=clamp(c,0.0,2.0);
    // ── Vignette
    vec2 vig=fragTexCoord*2.0-1.0;
    float v=1.0-dot(vig*0.52,vig*0.52);
    c*=smoothstep(0.0,1.0,v);
    // ── ACES Tone Mapping (hard-coded constants, stable across all frames)
    // Narkowicz ACES fit: no frame-dependent luminance adaptation
    const float A=2.51, B=0.03, C=2.43, D=0.59, E=0.14;
    c=(c*(A*c+B))/(c*(C*c+D)+E);
    c=clamp(c,0.0,1.0);
    finalColor=vec4(c,1.0);
})";

// Noise-based sandstorm
static const char *FS_SANDSTORM = R"(
#version 330
in vec2 fragTexCoord; in vec4 fragColor;
uniform sampler2D texture0; uniform vec4 colDiffuse;
uniform float time; uniform vec2 resolution;
out vec4 finalColor;
vec3 mod289(vec3 x){return x-floor(x*(1.0/289.0))*289.0;}
vec2 mod289(vec2 x){return x-floor(x*(1.0/289.0))*289.0;}
vec3 permute(vec3 x){return mod289(((x*34.0)+1.0)*x);}
float snoise(vec2 v){
    const vec4 C=vec4(0.211325,0.366025,-0.577350,0.024390);
    vec2 i=floor(v+dot(v,C.yy)); vec2 x0=v-i+dot(i,C.xx);
    vec2 i1=(x0.x>x0.y)?vec2(1,0):vec2(0,1);
    vec4 x12=x0.xyxy+C.xxzz; x12.xy-=i1;
    i=mod289(i);
    vec3 p=permute(permute(i.y+vec3(0,i1.y,1))+i.x+vec3(0,i1.x,1));
    vec3 m=max(0.5-vec3(dot(x0,x0),dot(x12.xy,x12.xy),dot(x12.zw,x12.zw)),0.0);
    m=m*m; m=m*m;
    vec3 x=2.0*fract(p*C.www)-1.0; vec3 h=abs(x)-0.5;
    vec3 ox=floor(x+0.5); vec3 a0=x-ox;
    m*=1.79284-0.85373*(a0*a0+h*h);
    vec3 g; g.x=a0.x*x0.x+h.x*x0.y; g.yz=a0.yz*x12.xz+h.yz*x12.yw;
    return 130.0*dot(m,g);
}
void main(){
    vec2 uv=fragTexCoord;
    vec2 st=uv*vec2(resolution.x/resolution.y,1.0)*4.0;
    float wind=time*0.8;
    // Multi-octave noise for organic dust wisp shapes
    float n1=snoise(st+vec2(wind,time*0.1))*0.5+0.5;
    float n2=snoise(st*2.0+vec2(wind*1.3,-time*0.15))*0.5+0.5;
    float n3=snoise(st*0.5+vec2(wind*0.6,time*0.05))*0.5+0.5;
    float n4=snoise(st*3.0+vec2(wind*1.8,time*0.25))*0.5+0.5;
    float dust=n1*0.40+n2*0.30+n3*0.18+n4*0.12;
    dust=smoothstep(0.30,0.80,dust);
    // Horizontal wind streaks (elongated wisps, not round)
    float streak=snoise(vec2(st.x*0.3+wind*2.5,st.y*4.0))*0.5+0.5;
    dust*=mix(0.5,1.0,streak);
    // Sine-wave swirl for organic movement
    float swirl=sin(st.y*6.0+time*2.0+st.x*0.5)*0.15;
    dust+=snoise(st+vec2(wind+swirl,time*0.3))*0.12;
    dust=clamp(dust,0.0,1.0);
    // Warm golden dust with slight color variation
    float rVar=0.82+0.04*sin(st.x*3.0+time);
    float gVar=0.68+0.03*cos(st.y*2.0+time*0.7);
    finalColor=vec4(rVar,gVar,0.39,dust*0.13);
})";

// ── Pixelation shader — downsamples to pixel-art resolution ─────────────────
static const char *FS_PIXELATE = R"(
#version 330
in vec2 fragTexCoord; in vec4 fragColor;
uniform sampler2D texture0; uniform vec4 colDiffuse;
uniform vec2 resolution;  // target pixel-art resolution (e.g. 320x213)
out vec4 finalColor;
void main(){
    vec2 pixelSize = 1.0 / resolution;
    vec2 uv = floor(fragTexCoord * resolution) / resolution + pixelSize * 0.5;
    finalColor = texture(texture0, uv) * colDiffuse * fragColor;
})";

// ── HD-2D Sprite Lighting shader (directional sun on pixel-art billboard) ────
static const char *VS_SPRITE_LIT = R"(
#version 330
layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
void main(){
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
})";

static const char *FS_SPRITE_LIT = R"(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float sunLR;
out vec4 finalColor;
void main(){
    vec4 tex = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    if(tex.a < 0.1) discard;
    // ── Horizontal directional lighting (sun side vs shadow side)
    // sunLR > 0 → sun from right → UV.x=1 is lit
    float hSide = (sunLR > 0.0) ? fragTexCoord.x : (1.0 - fragTexCoord.x);
    float hLight = mix(0.55, 1.0, hSide);
    // ── Vertical: sun from above → top slightly brighter
    float vLight = mix(0.88, 1.08, 1.0 - fragTexCoord.y);
    float L = hLight * vLight;
    // ── Warm desert sun + cool ambient fill
    vec3 sunCol = vec3(1.15, 1.05, 0.85);
    vec3 ambient = vec3(0.45, 0.38, 0.30);
    vec3 lit = tex.rgb * mix(ambient, sunCol, L);
    // ── Rim light: bright edge on shadow side for "3D volume" feel
    // Rim is strongest at the silhouette edge (near UV x=0 or x=1)
    float rimSide = (sunLR > 0.0) ? (1.0 - fragTexCoord.x) : fragTexCoord.x;
    float rimEdge = smoothstep(0.0, 0.15, rimSide) * (1.0 - smoothstep(0.15, 0.30, rimSide));
    // Also rim on top edge
    float rimTop = smoothstep(0.0, 0.10, fragTexCoord.y) * (1.0 - smoothstep(0.10, 0.25, fragTexCoord.y));
    float rim = max(rimEdge, rimTop * 0.6);
    vec3 rimCol = vec3(0.95, 0.85, 0.65); // warm backlight
    lit += rimCol * rim * 0.35;
    finalColor = vec4(lit, tex.a);
})";

// ── Triplanar sand shader with micro-detail normal influence ───────────────
static const char *VS_TRIPLANAR = R"(
#version 330
layout(location=0) in vec3 vertexPosition;
layout(location=1) in vec2 vertexTexCoord;
layout(location=2) in vec3 vertexNormal;
layout(location=3) in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
out vec3 fragWorldPos;
out vec3 fragNormal;
out vec4 fragColor;
void main(){
    fragWorldPos = vec3(matModel * vec4(vertexPosition, 1.0));
    fragNormal = normalize(vec3(matModel * vec4(vertexNormal, 0.0)));
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

static const char *FS_TRIPLANAR = R"(
#version 330
in vec3 fragWorldPos;
in vec3 fragNormal;
in vec4 fragColor;
uniform sampler2D texture0; // sand albedo
uniform sampler2D texture1; // ripples normal map
uniform sampler2D texture2; // pixel-grit overlay
uniform float sandScale;    // tiling scale
uniform vec3 sunDir;       // directional light (world)
uniform vec3 cameraPos;    // rim light view vector
uniform vec4 colDiffuse;
out vec4 finalColor;

vec4 sampleTriPlanar(sampler2D tex, vec3 pos, vec3 n, float scale){
    vec3 an = abs(n);
    an = pow(an, vec3(4.0)); // sharpen weights to reduce seams
    an /= (an.x + an.y + an.z + 1e-5);
    vec2 uvX = pos.zy * scale;
    vec2 uvY = pos.xz * scale;
    vec2 uvZ = pos.xy * scale;
    vec4 tx = texture(tex, uvX);
    vec4 ty = texture(tex, uvY);
    vec4 tz = texture(tex, uvZ);
    return tx * an.x + ty * an.y + tz * an.z;
}

void main(){
    vec3 n = normalize(fragNormal);
    vec4 albedo = sampleTriPlanar(texture0, fragWorldPos, n, sandScale);
    vec4 nm = sampleTriPlanar(texture1, fragWorldPos, n, sandScale * 2.0);
    vec4 grit = sampleTriPlanar(texture2, fragWorldPos, n, sandScale * 0.5);
    float ripple = nm.r * 0.5 + nm.g * 0.5;
    float micro = mix(0.92, 1.08, ripple);
    vec3 base = albedo.rgb * micro;
    // Cel-shaded lighting (2 bands)
    float L = max(dot(n, -sunDir), 0.0);
    float band = (L > 0.5) ? 1.0 : 0.55; // Bright vs Shadow sand
    // Rim highlight on crests (view-dependent)
    vec3 V = normalize(cameraPos - fragWorldPos);
    float rim = pow(clamp(1.0 - max(dot(n, V), 0.0), 0.0, 1.0), 3.0);
    rim *= smoothstep(0.6, 0.95, L); // only on lit side near crest
    vec3 rimCol = vec3(1.15, 1.05, 0.85);
    // Pixel-grit overlay (two-tone), low frequency to avoid moiré
    float gritMask = grit.r;
    vec3 gritCol = mix(vec3(0.92, 0.80, 0.60), vec3(0.70, 0.55, 0.40), step(0.5, gritMask));
    vec3 color = base * band;
    color = mix(color, color * rimCol, rim * 0.35);
    color = mix(color, gritCol, 0.08);
    // Vertex color contribution (AO valley darkening baked in C++)
    color *= colDiffuse.rgb * fragColor.rgb / vec3(255.0, 255.0, 255.0);
    finalColor = vec4(color, 1.0);
}
)";
// ── Post-processing state ────────────────────────────────────────────────────
static RenderTexture2D g_sceneFBO, g_brightFBO, g_blurA, g_blurB, g_godrayFBO,
    g_pixelFBO;
static Shader g_shBloomExtract, g_shBlur, g_shGodrays, g_shComposite,
    g_shSandstorm;
static Shader g_shSpriteLit;
static int g_locSpriteSunLR;
static Shader g_shPixelate;
static int g_locPixelRes;
static int g_locThreshold, g_locBlurDir, g_locSunPos;
static int g_locBloomTex, g_locGodrayTex, g_locBloomStr, g_locGodrayStr;
static int g_locStormTime, g_locStormRes;
static int g_locCompTime, g_locCompExposure, g_locCompRes;
static bool g_highFidelityMode = true;
static Shader g_shTriplanar;
static int g_locSandScale;
static int g_locTripSunDir, g_locTripCameraPos;

// ── Constants
// ─────────────────────────────────────────────────────────────────
static constexpr int SCREEN_W = 960;
static constexpr int SCREEN_H = 640;
static constexpr int MAP_W = 40;
static constexpr int MAP_H = 30;
static constexpr int INT_W = 10;
static constexpr int INT_H = 8;
static constexpr float TILE_W = 1.0f; // 1 world unit = 1 tile
static constexpr float PLAYER_SPEED = 5.0f;    // world units/sec (fluid analog)
static constexpr float PLAYER_RADIUS = 0.30f;  // sphere collider radius
static constexpr float ROTATION_TIME = 0.10f;  // seconds for smooth slerp (0.1s)
static constexpr int SPR_W = 24;
static constexpr int SPR_H = 32;
static constexpr float SPR_SCALE = 1.0f; // player fits 1×1 tile
static constexpr int MAX_PARTICLES = 120;
static constexpr int TENT_SIZE = 4;      // tents are 4×4 tiles
static constexpr float EXPOSURE = 1.45f; // overexposed desert sun
// Sun direction for shadow projection (from top-right)
static constexpr float SUN_DX = -0.5f;
static constexpr float SUN_DZ = 0.35f;

enum Dir { DIR_DOWN = 0, DIR_UP, DIR_LEFT, DIR_RIGHT, DIR_COUNT };
static constexpr int FRAMES_PER_DIR = 3;
static constexpr int TOTAL_FRAMES = DIR_COUNT * FRAMES_PER_DIR;
// Forward declaration — NPC facing direction based on player proximity
static Dir GetNPCPlayerDir(Vector3 npcPos);

// ── Post-processing init/cleanup ─────────────────────────────────────────────
static void InitPostProcessing() {
  g_sceneFBO = LoadRenderTexture(SCREEN_W, SCREEN_H);
  g_pixelFBO = LoadRenderTexture(SCREEN_W, SCREEN_H);
  g_brightFBO = LoadRenderTexture(SCREEN_W / 2, SCREEN_H / 2);
  g_blurA = LoadRenderTexture(SCREEN_W / 2, SCREEN_H / 2);
  g_blurB = LoadRenderTexture(SCREEN_W / 2, SCREEN_H / 2);
  g_godrayFBO = LoadRenderTexture(SCREEN_W / 2, SCREEN_H / 2);
  g_shBloomExtract = LoadShaderFromMemory(nullptr, FS_BLOOM_EXTRACT);
  g_shBlur = LoadShaderFromMemory(nullptr, FS_BLUR);
  g_shGodrays = LoadShaderFromMemory(nullptr, FS_GODRAYS);
  g_shComposite = LoadShaderFromMemory(nullptr, FS_COMPOSITE);
  g_shSandstorm = LoadShaderFromMemory(nullptr, FS_SANDSTORM);
  g_locThreshold = GetShaderLocation(g_shBloomExtract, "threshold");
  g_locBlurDir = GetShaderLocation(g_shBlur, "direction");
  g_locSunPos = GetShaderLocation(g_shGodrays, "sunPos");
  g_locBloomTex = GetShaderLocation(g_shComposite, "bloomTex");
  g_locGodrayTex = GetShaderLocation(g_shComposite, "godrayTex");
  g_locBloomStr = GetShaderLocation(g_shComposite, "bloomStr");
  g_locGodrayStr = GetShaderLocation(g_shComposite, "godrayStr");
  g_locCompTime = GetShaderLocation(g_shComposite, "time");
  g_locCompExposure = GetShaderLocation(g_shComposite, "exposure");
  g_locCompRes = GetShaderLocation(g_shComposite, "resolution");
  g_locStormTime = GetShaderLocation(g_shSandstorm, "time");
  g_locStormRes = GetShaderLocation(g_shSandstorm, "resolution");
  // ── HD-2D sprite lighting shader ─────────────────────────────────────────
  g_shSpriteLit = LoadShaderFromMemory(VS_SPRITE_LIT, FS_SPRITE_LIT);
  g_locSpriteSunLR = GetShaderLocation(g_shSpriteLit, "sunLR");
  // ── Pixelation shader ─────────────────────────────────────────────────────
  g_shPixelate = LoadShaderFromMemory(nullptr, FS_PIXELATE);
  g_locPixelRes = GetShaderLocation(g_shPixelate, "resolution");
  // ── Triplanar terrain shader ───────────────────────────────────────────────
  g_shTriplanar = LoadShaderFromMemory(VS_TRIPLANAR, FS_TRIPLANAR);
  g_locSandScale = GetShaderLocation(g_shTriplanar, "sandScale");
  g_locTripSunDir = GetShaderLocation(g_shTriplanar, "sunDir");
  g_locTripCameraPos = GetShaderLocation(g_shTriplanar, "cameraPos");
}
static void CleanupPostProcessing() {
  UnloadRenderTexture(g_sceneFBO);
  UnloadRenderTexture(g_pixelFBO);
  UnloadRenderTexture(g_brightFBO);
  UnloadRenderTexture(g_blurA);
  UnloadRenderTexture(g_blurB);
  UnloadRenderTexture(g_godrayFBO);
  UnloadShader(g_shBloomExtract);
  UnloadShader(g_shBlur);
  UnloadShader(g_shGodrays);
  UnloadShader(g_shComposite);
  UnloadShader(g_shSandstorm);
  UnloadShader(g_shSpriteLit);
  UnloadShader(g_shPixelate);
  UnloadShader(g_shTriplanar);
}
static void DrawFBOQuad(RenderTexture2D fbo) {
  DrawTextureRec(fbo.texture,
                 {0, 0, (float)fbo.texture.width, -(float)fbo.texture.height},
                 {0, 0}, WHITE);
}
static void DrawFBOQuadScaled(RenderTexture2D fbo, int dstW, int dstH) {
  Rectangle src = {0, 0, (float)fbo.texture.width, -(float)fbo.texture.height};
  Rectangle dst = {0, 0, (float)dstW, (float)dstH};
  DrawTexturePro(fbo.texture, src, dst, {0, 0}, 0, WHITE);
}

// ── Scene IDs
// ─────────────────────────────────────────────────────────────────
enum Scene {
  SCENE_OVERWORLD = 0,
  SCENE_TENT1,
  SCENE_TENT2,
  SCENE_TENT3,
  SCENE_TENT4,
  SCENE_MATCH,
  SCENE_SHOP
};

// ── Structures
// ────────────────────────────────────────────────────────────────
struct SubCube {
  Vector3 offset, scale;
  float rotY;
};
struct Rock {
  int gx, gy;
  float worldX, worldZ;
  std::vector<SubCube> cubes;
  Color baseCol;
};
struct Tent {
  int gx, gy;
  Color wallCol, roofCol;
  const char *signText;
  int doorGX, doorGY;
};
// ── 3D Chibi Model color palette (BDSP vinyl figure aesthetic) ──────────────
struct ChibiColors {
  Color skin, skinHi, skinSh;   // Base / highlight / shadow
  Color hair, hairHi;           // Hair mass + gloss
  Color eye, eyeIris;           // Pupil + iris
  Color cap, capHi;             // Hat/cap + highlight
  Color jacket, jacketHi, jacketSh; // Torso
  Color pants, pantsSh;         // Legs
  Color shoe, shoeHi;           // Boots
  Color outline;                // Edge tint (used for eyelashes etc.)
};

struct NPC {
  int gx, gy;
  float worldX, worldZ;
  const char *name;
  Color shirtCol, pantsCol, hatCol;
  Dir dir;
  // ── 3D rotation state ──
  float facingAngle;    // current Y-rotation in degrees (0=south, 90=west, etc.)
  float targetAngle;    // desired Y-rotation
  ChibiColors colors;   // vinyl figure colors
};
struct Particle {
  float x, y, vx, vy, life, maxLife, size;
  unsigned char alpha;
};
struct Player {
  // ── Fluid position (no grid) ──
  float posX, posZ;       // continuous world-space position
  float velX, velZ;       // current frame velocity (units/sec)
  float colliderRadius;   // sphere collider radius for wall sliding
  bool moving;            // true when velocity > 0 (drives walk animation)
  // ── Facing / animation ──
  float facingAngle;      // current Y-rotation in degrees (smoothly interpolated)
  float targetAngle;      // desired Y-rotation from velocity vector
  float animTimer;        // walk/idle animation accumulator
  int animFrame;          // legacy compat (kept for card game scene)
  Dir dir;                // cardinal direction hint (for NPC interaction etc.)
  // ── Scene ──
  Vector3 lastOverworldPos;
  Vector2 interiorPos;
  // ── Deprecated grid compat (kept for collision map lookups) ──
  int gridX, gridY;       // snapped to nearest tile for collision map reads
};
// ── Trigger Zone: world-space door triggers with precise alignment ───────────
struct TriggerZone {
  float minX, minZ, maxX, maxZ; // world-space bounding box (ground plane)
  Scene targetScene;            // where to teleport
  int destGridX, destGridY;     // destination tile
  Dir destDir;                  // facing direction after teleport
  Dir requiredDir;              // player must face this direction to trigger
};

// ── Globals
// ───────────────────────────────────────────────────────────────────
static Camera3D g_cam;
static Camera3D matchCam;
static Player g_player;
static ChibiColors g_playerColors; // 3D chibi palette (initialized in InitOverworld)
static Texture2D g_sandTex, g_stormTex, g_torchGlow, g_signTextures[4];
static std::vector<Tent> g_tents;
static std::vector<Rock> g_rocks;
static NPC g_npcs[4];
static float g_time = 0.0f;
static Scene g_scene = SCENE_OVERWORLD;
static int g_savedGridX = 0, g_savedGridY = 0;
static Dir g_savedDir = DIR_DOWN;
static int g_collision[MAP_H][MAP_W];
static Particle g_particles[MAX_PARTICLES];
static int g_particleCount = 0;
static int g_intCollision[INT_H][INT_W];
static std::vector<TriggerZone> g_triggerZones; // overworld entrance triggers
static TriggerZone g_exitZone;                  // interior exit trigger
// (g_spriteSunLR removed — no more billboards)
static float g_prevPlayerX =
    0.0f; // previous frame player X (for pan compensation)

// ── Controller globals ──
static bool g_gamepadConnected = false;
static float g_dpadX = 0.0f, g_dpadY = 0.0f;
static bool g_padStart = false, g_padA = false, g_padB = false;
static bool g_padStartPressed = false, g_padAPressed = false,
            g_padBPressed = false;

static constexpr float SCENE_FADE_DURATION = 0.6f;
static constexpr float INTERIOR_MOVE_SPEED = 4.8f;
static constexpr float FIXED_WIND_SPEED = 450.0f;


static Vector3 ResolveOverworldReturnPos(Vector3 desired) {
  int gx = (int)roundf(desired.x);
  int gy = (int)roundf(desired.z);
  gx = Clamp(gx, 0, MAP_W - 1);
  gy = Clamp(gy, 0, MAP_H - 1);
  if (g_collision[gy][gx] == 0)
    return {(float)gx, 0.0f, (float)gy};
  for (int r = 1; r <= 4; r++) {
    for (int dz = -r; dz <= r; dz++) {
      for (int dx = -r; dx <= r; dx++) {
        int tx = gx + dx, ty = gy + dz;
        if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H)
          continue;
        if (g_collision[ty][tx] == 0)
          return {(float)tx, 0.0f, (float)ty};
      }
    }
  }
  return {(float)g_player.gridX, 0.0f, (float)g_player.gridY};
}

class SceneManager {
public:
  void LoadScene(Scene target, int tentId = -1) {
    m_pendingScene = target;
    m_pendingTentId = tentId;
    m_phase = PHASE_FADE_OUT;
    m_timer = 0.0f;
  }

  void Update(float dt) {
    if (m_phase == PHASE_IDLE)
      return;
    float half = SCENE_FADE_DURATION * 0.5f;
    m_timer += dt;
    if (m_phase == PHASE_FADE_OUT && m_timer >= half) {
      ApplySceneSwap();
      m_phase = PHASE_FADE_IN;
      m_timer = 0.0f;
    } else if (m_phase == PHASE_FADE_IN && m_timer >= half) {
      m_phase = PHASE_IDLE;
      m_timer = 0.0f;
    }
  }

  bool IsTransitioning() const { return m_phase != PHASE_IDLE; }

  void DrawFadeOverlay() const {
    if (m_phase == PHASE_IDLE)
      return;
    float half = SCENE_FADE_DURATION * 0.5f;
    float t = Clamp(m_timer / half, 0.0f, 1.0f);
    float a = (m_phase == PHASE_FADE_OUT) ? t : (1.0f - t);
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H,
                  {0, 0, 0, (unsigned char)(a * 255.0f)});
  }

private:
  enum Phase { PHASE_IDLE = 0, PHASE_FADE_OUT, PHASE_FADE_IN };

  void ApplySceneSwap() {
    g_scene = m_pendingScene;
    if (g_scene == SCENE_OVERWORLD) {
      Vector3 desired =
          Vector3Subtract(g_player.lastOverworldPos, {0.0f, 0.0f, 1.5f});
      Vector3 spawn = ResolveOverworldReturnPos(desired);
      g_player.gridX = (int)roundf(spawn.x);
      g_player.gridY = (int)roundf(spawn.z);
      g_player.posX = (float)g_player.gridX;
      g_player.posZ = (float)g_player.gridY;
      g_player.velX = g_player.velZ = 0;
      g_player.moving = false;
      g_player.dir = DIR_DOWN;
      g_cam.position = {g_player.posX, 14.0f, g_player.posZ + 10.0f};
      g_cam.target = {g_player.posX, GetDuneHeight(g_player.posX, g_player.posZ), g_player.posZ};
    }
  }

  Scene m_pendingScene = SCENE_OVERWORLD;
  int m_pendingTentId = -1;
  Phase m_phase = PHASE_IDLE;
  float m_timer = 0.0f;
};

static SceneManager g_sceneManager;

class BaseScene {
public:
  virtual ~BaseScene() = default;
  virtual void Update(float dt) = 0;
  virtual void Draw() = 0;
};

class TentInterior : public BaseScene {
public:
  void Update(float dt) override;
  void Draw() override;
  void DrawInterior();
};

static TentInterior g_tentInterior;

static void UpdateController(float dt) {
  (void)dt;
  g_gamepadConnected = IsGamepadAvailable(0);
  if (g_gamepadConnected) {
    g_dpadX = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
    g_dpadY = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
    g_padStart = IsGamepadButtonDown(0, GAMEPAD_BUTTON_MIDDLE_RIGHT); // Start
    g_padA = IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);  // A
    g_padB = IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT); // B
    g_padStartPressed =
        IsGamepadButtonPressed(0, GAMEPAD_BUTTON_MIDDLE_RIGHT); // Start
    g_padAPressed = IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    g_padBPressed = IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
  } else {
    g_dpadX = g_dpadY = 0;
    g_padStart = g_padA = g_padB = false;
    g_padStartPressed = g_padAPressed = g_padBPressed = false;
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SOVEREIGN HORIZONS — Card Game System
// ═══════════════════════════════════════════════════════════════════════════════

// ── Card Definitions ────────────────────────────────────────────────────────
struct CardDef {
  int id;
  const char *name;
  int cost;
  bool isUnit;
  const char *subtype;
  int atk, def;
  const char *keywords; // semicolon-separated: "fly;dash;tenacity;overrun"
  const char *effect;   // text description
  int rarity;           // 0=common, 1=rare
  bool isUnique;        // unique (one per deck)
};

static const CardDef ALL_CARDS[] = {
    // ══════════════════════ COMMON UNITS (id 1-107 from CSV)
    // ══════════════════
    // Cost 1
    {1, "Young Thief", 1, true, "rogue", 4, 2, "tenacity", "", 0, false},
    {2, "Fast Rat", 1, true, "rat", 4, 1, "dash", "", 0, false},
    {3, "Red Rose Warrior", 1, true, "plant", 0, 7, "", "", 0, false},
    {4, "Mud Golem", 1, true, "golem", 2, 4, "tenacity", "", 0, false},
    {5, "Giant Ant", 1, true, "bug", 4, 3, "", "", 0, false},
    {6, "Mad Warrior", 1, true, "berserker", 5, 2, "", "", 0, false},
    {7, "Sorcerer Apprentice", 1, true, "mage", 3, 4, "", "", 0, false},
    {8, "Little Wurm", 1, true, "wurm", 4, 3, "", "", 0, false},
    {9, "Ephemeral Angel", 1, true, "angel", 3, 2, "fly", "", 0, false},
    {10, "Desert Nomad", 1, true, "merchant", 2, 5, "", "", 0, false},
    {11, "Hasty Fiend", 1, true, "demon", 3, 2, "dash", "", 0, false},
    {12, "Flying Ghost", 1, true, "spirit", 2, 3, "fly", "", 0, false},
    {13, "Wild Beast", 1, true, "beast", 4, 1, "overrun", "", 0, false},
    {14, "Brave Soldier", 1, true, "soldier", 4, 3, "", "", 0, false},
    {15, "Undead Servant", 1, true, "zombie", 4, 3, "", "", 0, false},
    {16, "Quick Rat", 1, true, "rat", 4, 0, "dash",
     "Enter - Opponent discards 1 card from hand.", 0, false},
    {17, "Desert Rat", 1, true, "rat", 3, 2, "",
     "Enter/Attack - Opponent loses 3 life.", 0, false},
    {18, "Plague Rat", 1, true, "rat", 2, 3, "",
     "Enter/Death - Put a weak counter on each enemy.", 0, false},
    {19, "Digging Rat", 1, true, "rat", 3, 1, "", "Enter/Attack - Dig 3.", 0,
     false},
    {20, "Garden Keeper", 1, true, "plant", 0, 5, "", "Harvest - Gain 5 life.",
     0, false},
    {21, "Yellow Rose Seed", 1, true, "plant", 0, 4, "",
     "Harvest - Discard a card then gain 1 coin.", 0, false},
    {22, "Blue Rose Seed", 1, true, "plant", 0, 4, "",
     "Harvest - Discard a card then draw 1 card.", 0, false},
    {23, "Stone Golem", 1, true, "golem", 3, 3, "",
     "Closure - You may have this unit become a defender.", 0, false},
    {24, "Rock Golem", 1, true, "golem", 3, 4, "",
     "Defend - Put 2 power counters on this unit.", 0, false},
    {25, "Slaying Mantis", 1, true, "bug", 5, 3, "fly",
     "Attack - You lose 3 life.", 0, false},
    {26, "Dark Caterpillar", 1, true, "bug", 3, 3, "",
     "Enter - Put 3 weak counters on an enemy.", 0, false},
    {27, "Angry Pacifist", 1, true, "berserker", 6, 3, "dash",
     "Closure - You lose 3 life.", 0, false},
    {28, "Furious Avenger", 1, true, "berserker", 7, 5, "overrun",
     "Closure - Discard a card.", 0, false},
    {29, "Fire Apprentice", 1, true, "mage", 2, 2, "",
     "Enter/Ability - Deal 2 damage.", 0, false},
    {30, "Ice Apprentice", 1, true, "mage", 2, 3, "",
     "Enter/Ability - Draw 1 card then discard 1 card.", 0, false},
    {31, "Dark Apprentice", 1, true, "mage", 3, 3, "",
     "Enter/Ability - Opponent loses 3 life.", 0, false},
    {32, "Nomadic Salesman", 1, true, "merchant", 2, 3, "",
     "Enter/Ability - Discard 2 cards then draw 1 card and gain 1 coin.", 0,
     false},
    {33, "Travelling Vendor", 1, true, "merchant", 2, 3, "",
     "Enter/Ability - Pay 1 coin to draw 2 cards then discard 1 card.", 0,
     false},
    {34, "Ruthless Negotiator", 1, true, "merchant", 4, 3, "tenacity",
     "Enter/Attack - Put a weak counter on an ally of your choice to gain 1 "
     "coin.",
     0, false},
    {35, "Dead Demon of Discord", 1, true, "demon", 3, 1, "dash",
     "Death - Draw 1 card and you lose 3 life.", 0, false},
    {36, "Dead Demon of Dreams", 1, true, "demon", 3, 1, "dash",
     "Death - Gain 1 coin and you lose 3 life.", 0, false},
    {37, "Dead Demon of Doubt", 1, true, "demon", 3, 1, "dash",
     "Death - Opponent discards 1 card then you lose 3 life.", 0, false},
    {38, "Dead Demon of Darkness", 1, true, "demon", 3, 1, "dash",
     "Death - Deal 3 damage and you lose 3 life.", 0, false},
    {39, "Dead Demon of Deceit", 1, true, "demon", 3, 1, "dash",
     "Death - Add 2 demons from your graveyard to your hand then you lose 3 "
     "life.",
     0, false},
    {40, "Life Guardian", 1, true, "spirit", 3, 1, "fly",
     "Enter/Attack/Death - Gain 3 life.", 0, false},
    {41, "Savage Boar", 1, true, "beast", 4, 2, "", "Enter - Gain 4 life.", 0,
     false},
    {42, "Angry Beast", 1, true, "beast", 5, 4, "dash;overrun",
     "Attack - Discard a card.", 0, false},
    {43, "Prodigious Recruit", 1, true, "soldier", 3, 3, "",
     "Attack - Put 2 power counters on this unit.", 0, false},
    {44, "Lucky Recruit", 1, true, "soldier", 2, 4, "",
     "Defend - Put 2 power counters on another ally.", 0, false},
    {45, "Undead Horde", 1, true, "zombie", 6, 0, "overrun",
     "Passive - Cannot defend.", 0, false},
    {46, "Dead Walkers", 1, true, "zombie", 6, 2, "",
     "Passive - Cannot defend.", 0, false},
    // Cost 2
    {47, "Special Tactics Unit", 2, true, "rogue", 7, 4, "tenacity;dash",
     "Attack - Draw 2 cards then discard 2 cards.", 0, false},
    {48, "Close-Combat Expert", 2, true, "rogue", 4, 4, "tenacity",
     "Attack - Put 4 power counters on this unit.", 0, false},
    {49, "Giant Rat", 2, true, "rat", 6, 6, "",
     "Attack - Opponent discards 1 card then loses 3 life.", 0, false},
    {50, "Cursed Flower", 2, true, "plant", 0, 7, "",
     "Harvest - Gain 1 coin and you lose 1 life.", 0, false},
    {51, "Flower Pollinator", 2, true, "plant", 0, 6, "",
     "Harvest - Make a 0/2 plant token then gain 2 life.", 0, false},
    {52, "Persistent Golem", 2, true, "golem", 7, 7, "tenacity",
     "Death - Add this card to hand.", 0, false},
    {53, "Dark Chrysalis", 2, true, "bug", 0, 10, "",
     "Rush - Sacrifice Dark Caterpillar to deploy this unit. If you do draw 2 "
     "cards.",
     0, false},
    {54, "Giant Spider", 2, true, "bug", 8, 8, "",
     "Attack/Defend - Make a 2/2 bug token.", 0, false},
    {55, "Experimental Failure", 2, true, "berserker", 12, 8, "dash;overrun",
     "Enter - Discard 2 cards then you lose 2 life.", 0, false},
    {56, "Curious Entomologist", 2, true, "mage", 4, 8, "",
     "Harvest - Make two 2/2 bug tokens and you lose 2 life.", 0, false},
    {57, "Arcane Archeologist", 2, true, "mage", 5, 8, "",
     "Enter - Choose support from your graveyard and add it to your hand.", 0,
     false},
    {58, "Skilled Pyromancer", 2, true, "mage", 3, 9, "",
     "Enter/Ability - Deal 3 damage.", 0, false},
    {59, "Lonely Trader", 2, true, "merchant", 5, 8, "",
     "Enter/Ability - Discard 1 card to gain 2 coins.", 0, false},
    {60, "Life Dealer", 2, true, "merchant", 3, 6, "",
     "Enter/Ability - Gain 1 coin then you lose 3 life.", 0, false},
    {61, "Beast Tamer", 2, true, "merchant", 4, 7, "",
     "Enter/Ability - Make a 3/3 beast token.", 0, false},
    {62, "Dealer Demon of Disease", 2, true, "demon", 6, 6, "fly",
     "Harvest - Sacrifice a unit then draw 1 card.", 0, false},
    {63, "Dealer Demon of Death", 2, true, "demon", 6, 6, "fly",
     "Harvest - Sacrifice a unit then your opponent chooses and discards 1 "
     "card.",
     0, false},
    {64, "Blessing Spirit", 2, true, "spirit", 4, 4, "fly",
     "Enter - Draw 1 card and gain 4 life.", 0, false},
    {65, "Savage Beast", 2, true, "beast", 8, 4, "",
     "Enter - Deal 4 damage to an enemy.", 0, false},
    {66, "Lurking Hunter", 2, true, "beast", 9, 6, "",
     "Attack - Deal 3 damage to each enemy.", 0, false},
    {67, "Aspiring Sergeant", 2, true, "soldier", 8, 4, "",
     "Enter - Put 4 power counters on another ally.", 0, false},
    {68, "Honorable Sergeant", 2, true, "soldier", 7, 7, "tenacity",
     "Attack - Put 2 power counters on each other ally.", 0, false},
    {69, "Heavy-Arms Officer", 2, true, "soldier", 10, 5, "dash",
     "Attack - Each player loses 5 life.", 0, false},
    {70, "Rotten Corpse", 2, true, "zombie", 13, 0, "overrun",
     "Passive - Cannot defend. Enter - Each player loses 3 life.", 0, false},
    // Cost 3
    {71, "Quick Thief", 3, true, "rogue", 10, 7, "dash",
     "Attack - Draw 3 cards then discard 2 cards.", 0, false},
    {72, "White Rose Guardian", 3, true, "plant", 0, 12, "",
     "Harvest - Draw 1 card gain 1 coin and gain 1 life.", 0, false},
    {73, "Black Rose Guardian", 3, true, "plant", 0, 12, "",
     "Harvest - Your opponent chooses and discards a card then they lose 3 "
     "life.",
     0, false},
    {74, "Silver Golem", 3, true, "golem", 9, 9, "", "Death - Gain 3 coins.", 0,
     false},
    {75, "Dark Imago", 3, true, "bug", 13, 8, "fly",
     "Rush - Sacrifice Dark Chrysalis to deploy this unit. If you do draw 3 "
     "cards.",
     0, false},
    {76, "Bloodrush Berserker", 3, true, "berserker", 13, 7, "dash;overrun",
     "Attack - You lose 4 life.", 0, false},
    {77, "Archmage of Storms", 3, true, "mage", 7, 11, "",
     "Enter/Ability - Deal 5 damage then draw 1 card.", 0, false},
    {78, "Gilded Caravan Master", 3, true, "merchant", 6, 10, "",
     "Enter/Ability - Gain 3 coins then draw 1 card.", 0, false},
    {79, "Abyssal Tormentor", 3, true, "demon", 10, 8, "fly",
     "Enter - Sacrifice a unit then deal 5 damage.", 0, false},
    {80, "Ancestral Wraith", 3, true, "spirit", 7, 8, "fly",
     "Enter/Death - Draw 2 cards and gain 4 life.", 0, false},
    {81, "Rampaging Hydra", 3, true, "beast", 12, 7, "overrun",
     "Attack - Deal 3 damage to an enemy.", 0, false},
    {82, "Veteran Commander", 3, true, "soldier", 9, 9, "tenacity",
     "Enter - Put 3 power counters on each other ally.", 0, false},
    {83, "Risen Abomination", 3, true, "zombie", 14, 5, "overrun",
     "Passive - Cannot defend. Enter - Put 3 weak counters on each enemy.", 0,
     false},
    // Cost 4
    {84, "Greedy Thief", 4, true, "rogue", 11, 9, "tenacity",
     "Attack - Gain 2 coins.", 0, false},
    {85, "Sneaky Thief", 4, true, "rogue", 11, 9, "tenacity",
     "Attack - Draw 2 cards.", 0, false},
    {86, "Eternal Blossom Tree", 4, true, "plant", 0, 15, "",
     "Harvest - Draw 2 cards gain 2 coins and gain 3 life.", 0, false},
    {87, "Adamantine Golem", 4, true, "golem", 12, 12, "tenacity",
     "Enter - Become a defender. Defend - Put 3 power counters on this unit.",
     0, false},
    {88, "Devouring Swarm Queen", 4, true, "bug", 10, 10, "fly",
     "Enter - Make two 3/3 bug tokens.", 0, false},
    {89, "Frenzied Warlord", 4, true, "berserker", 16, 10, "dash;overrun",
     "Enter - Discard 3 cards.", 0, false},
    {90, "Grand Sorcerer", 4, true, "mage", 8, 12, "",
     "Enter/Ability - Deal 6 damage then draw 1 card.", 0, false},
    {91, "Archfiend of Ruin", 4, true, "demon", 12, 10, "fly;dash",
     "Enter - Sacrifice 2 units then draw 3 cards.", 0, false},
    // Cost 5 (unique)
    {92, "Primordial Bloom", 5, true, "plant", 0, 18, "",
     "Harvest - Draw 2 cards gain 3 coins and gain 5 life.", 0, true},
    {93, "Colossal Golem", 5, true, "golem", 14, 14, "tenacity",
     "Death - Add this card to hand then gain 5 coins.", 0, true},
    {94, "Titan Beetle", 5, true, "bug", 15, 12, "fly;overrun",
     "Attack - Make a 4/4 bug token.", 0, true},
    {95, "Doomhowl Ravager", 5, true, "berserker", 18, 12, "dash;overrun",
     "Attack - You lose 5 life. Enter - Discard 2 cards.", 0, true},
    {96, "Arcane Overlord", 5, true, "mage", 10, 14, "",
     "Enter/Ability - Deal 8 damage then draw 2 cards.", 0, true},
    {97, "Alpha Predator", 5, true, "beast", 16, 12, "overrun",
     "Enter - Deal 6 damage to an enemy then deal 3 damage to each other "
     "enemy.",
     0, true},
    // Cost 6 (unique)
    {98, "Tree of Abundance", 6, true, "plant", 0, 16, "",
     "Harvest - Gain 3 coins.", 0, true},
    {99, "Mythril Golem", 6, true, "golem", 12, 12, "overrun",
     "Death - Add this card to hand then gain 6 life.", 0, true},
    {100, "Abyssal Moth Empress", 6, true, "bug", 14, 12, "fly;overrun",
     "Attack - Opponent chooses and discards 2 cards.", 0, true},
    {101, "Crazed Annihilator", 6, true, "berserker", 17, 11, "dash;overrun",
     "Attack - Sacrifice an ally.", 0, true},
    {102, "Grand Arcanist", 6, true, "mage", 7, 14, "",
     "Enter/Ability - Deal 7 damage.", 0, true},
    {103, "Ancient Wurm", 6, true, "wurm", 14, 14, "overrun",
     "Attack - Deal 4 damage to each other unit.", 0, true},
    {104, "Seraphim of Grace", 6, true, "angel", 12, 14, "fly",
     "Enter - Gain 14 life.", 0, true},
    {105, "Opulent Grandmaster", 6, true, "merchant", 5, 12, "",
     "Enter/Ability - Discard a card then draw 2 cards and gain 2 coins.", 0,
     true},
    {106, "Sovereign of the Abyss", 6, true, "demon", 13, 13,
     "fly;dash;overrun;tenacity",
     "Enter - Sacrifice a unit then you lose 6 life.", 0, true},
    {107, "Primal Stampede Lord", 6, true, "beast", 15, 12, "overrun",
     "Attack - Each other ally gains overrun this turn.", 0, true},
    // ══════════════════════ COMMON SUPPORTS (id 108-140 from CSV)
    // ═════════════
    {108, "Arrow", 0, false, "", 0, 0, "", "Deal 2 damage.", 0, false},
    {109, "Scrying Shovel", 0, false, "", 0, 0, "", "Dig 5.", 0, false},
    {110, "Ember Wave", 0, false, "", 0, 0, "", "Deal 1 damage to each enemy.",
     0, false},
    {111, "Grow", 0, false, "", 0, 0, "", "Put 3 power counters on an ally.", 0,
     false},
    {112, "Obsession", 0, false, "", 0, 0, "",
     "Draw 1 card and you lose 4 life.", 0, false},
    {113, "Luck", 0, false, "", 0, 0, "", "Gain 1 coin.", 0, false},
    {114, "Arcane Insight", 1, false, "", 0, 0, "",
     "Draw 2 cards then discard 1 card.", 0, false},
    {115, "Grave Beckoning", 1, false, "", 0, 0, "",
     "Add a unit from your graveyard to hand.", 0, false},
    {116, "Fire Bolt", 1, false, "", 0, 0, "", "Deal 4 damage.", 0, false},
    {117, "Spark of Knowledge", 1, false, "", 0, 0, "",
     "Deal 1 damage then draw 1 card.", 0, false},
    {118, "Battle Blessing", 1, false, "", 0, 0, "",
     "Put a power counter on an ally then draw 1 card.", 0, false},
    {119, "Scorching Tempest", 1, false, "", 0, 0, "",
     "Deal 3 damage to each unit.", 0, false},
    {120, "Deep Meditation", 2, false, "", 0, 0, "", "Draw 2 cards.", 0, false},
    {121, "Golden Harvest", 2, false, "", 0, 0, "",
     "Gain 4 coins at end of turn.", 0, false},
    {122, "War Drums", 2, false, "", 0, 0, "",
     "Put 4 power counters on each ally.", 0, false},
    {123, "Annihilate", 2, false, "", 0, 0, "", "Destroy an enemy.", 0, false},
    {124, "Searing Judgment", 2, false, "", 0, 0, "",
     "Deal 4 damage then draw 1 card.", 0, false},
    {125, "Necromantic Rite", 2, false, "", 0, 0, "",
     "Deploy a unit with cost 4 or less from your graveyard.", 0, false},
    {126, "Cruel Edict", 3, false, "", 0, 0, "",
     "Your opponent sacrifices 2 units.", 0, false},
    {127, "Mind Shatter", 3, false, "", 0, 0, "",
     "Your opponent discards 2 cards.", 0, false},
    {128, "Wings of Valor", 3, false, "", 0, 0, "",
     "Put 3 power counters on each ally and they gain fly until end of turn.",
     0, false},
    {129, "Plague of Frailty", 3, false, "", 0, 0, "",
     "Put 5 weak counters on each enemy.", 0, false},
    {130, "Forbidden Scroll", 3, false, "", 0, 0, "",
     "Draw 4 cards then discard 2 cards.", 0, false},
    {131, "Terminate", 4, false, "", 0, 0, "",
     "Destroy an enemy then draw 1 card.", 0, false},
    {132, "Twin Inferno", 4, false, "", 0, 0, "",
     "Deal 8 damage to up to 2 enemies.", 0, false},
    {133, "Warcry of the Ancients", 4, false, "", 0, 0, "",
     "Put 7 power counters on each ally and they gain overrun until end of "
     "turn.",
     0, false},
    {134, "Wheel of Fate", 4, false, "", 0, 0, "",
     "Discard your hand then draw 7 cards.", 0, false},
    {135, "Mass Resurrection", 5, false, "", 0, 0, "",
     "Deploy up to 2 units with cost 4 or less from your graveyard.", 0, true},
    {136, "Rich People's Luck", 5, false, "", 0, 0, "", "Gain 7 coins.", 0,
     false},
    {137, "Cataclysmic Purge", 5, false, "", 0, 0, "",
     "Destroy up to 3 enemies.", 0, true},
    {138, "Sovereign's Decree", 6, false, "", 0, 0, "",
     "Draw 2 cards and gain 6 life then your opponent discards 2 cards and "
     "loses 6 life.",
     0, true},
    {139, "Hellfire Apocalypse", 6, false, "", 0, 0, "",
     "Deal 6 damage to each enemy then draw 1 card for each enemy destroyed "
     "this way.",
     0, true},
    {140, "Dimensional Rift", 6, false, "", 0, 0, "",
     "Deploy a unit from your hand then deploy a unit from your graveyard.", 0,
     true},
    // ══════════════════════ RARE UNITS (id 141-192) ══════════════════════════
    {141, "Shadow Blade", 2, true, "rogue", 8, 6, "dash;tenacity",
     "Enter - Target enemy stops defending. Attack - Draw 1 card and lose 1 "
     "life.",
     1, false},
    {142, "Mind Thief", 2, true, "rogue", 9, 6, "",
     "Enter - Look at your opponent's hand and discard a card from there, then "
     "that player loses life equal to that card's cost.",
     1, false},
    {143, "Rat Lord", 2, true, "rat", 9, 3, "dash",
     "Enter - Add a rat from your graveyard to hand. Passive - Other ally rats "
     "gain +5/+5 while this unit is on the field.",
     1, false},
    {144, "Rat Commander", 2, true, "rat", 7, 7, "dash",
     "Attack - Make two 1/1 rat tokens, then put a power counter on each other "
     "ally rat for each rat in your graveyard. Closure - If you attacked with "
     "3 or more rats this turn, draw 1 card.",
     1, false},
    {145, "Bloom Harvester", 2, true, "plant", 0, 8, "",
     "Harvest - Draw 2 cards, then discard 1 card and lose 2 life.", 1, false},
    {146, "Wilt Bringer", 2, true, "plant", 0, 10, "",
     "Harvest - Put 2 weak counters on each ally, then gain 2 coins.", 1,
     false},
    {147, "Verdant Guardian", 2, true, "plant", 0, 10, "",
     "Enter/Death/Harvest - Put 4 power counters on each other ally, then gain "
     "4 life.",
     1, false},
    {148, "Rot Bloom", 2, true, "plant", 0, 8, "",
     "Harvest - Put 2 weak counters on each enemy, then destroy each enemy "
     "with 6 or more weak counters on it.",
     1, false},
    {149, "Golden Golem", 2, true, "golem", 7, 9, "tenacity",
     "Attack - Put a gold counter on this unit, then gain 1 coin for each gold "
     "counter on it.",
     1, false},
    {150, "War Golem", 2, true, "golem", 8, 8, "dash",
     "Enter - Put 4 power counters on each other ally and you gain 4 life. "
     "Death - Put 4 weak counters on each enemy and your opponent loses 4 "
     "life.",
     1, false},
    {151, "Hivemind Matriarch", 2, true, "bug", 4, 4, "fly",
     "Passive - Ally bugs have fly. Enter/Ability - Make two 2/2 bug tokens, "
     "then put 2 power counters on each ally bug.",
     1, false},
    {152, "Grave Moth", 2, true, "bug", 7, 7, "fly",
     "Enter - Draw 2 cards, then discard 2 cards.", 1, false},
    {153, "Battle Enchantress", 2, true, "mage", 2, 7, "",
     "Ability - Another ally gains +7 atk, fly, dash and overrun until the end "
     "of turn.",
     1, false},
    {154, "Arcane Striker", 2, true, "mage", 5, 5, "",
     "Enter - For each card in your hand, put 1 power counter on this unit, "
     "then deal that much damage to an enemy.",
     1, false},
    {155, "Shrewd Broker", 2, true, "merchant", 4, 8, "trade",
     "Ability - Pay 2 life and discard 2 cards to draw 3 cards and gain 1 "
     "coin.",
     1, false},
    {156, "Resourceful Peddler", 2, true, "merchant", 2, 8, "trade",
     "Ability - If you have no coins, gain 2 coins, then if you have no cards "
     "in hand, draw 1 card.",
     1, false},
    {157, "Abyssal Tyrant", 2, true, "demon", 9, 9, "dash;fly;overrun",
     "Passive - You cannot play units from hand. Enter - Sacrifice all other "
     "ally units.",
     1, false},
    {158, "Spite Fiend", 2, true, "demon", 5, 3, "dash",
     "Attack/Death - Your opponent must choose to sacrifice a unit or discard "
     "2 cards or lose 8 life.",
     1, false},
    // Cost 3 rare
    {159, "Master Infiltrator", 3, true, "rogue", 8, 8, "dash",
     "Passive - Can ignore defenders. Attack - Your opponent discards 1 card "
     "and you draw 1 card.",
     1, false},
    {160, "Elder Blossom", 3, true, "plant", 0, 12, "",
     "Harvest - Gain 2 coins and draw 1 card, then you lose 3 life.", 1, false},
    {161, "Titanic Golem", 3, true, "golem", 10, 10, "",
     "Enter - Draw 1 card and gain 5 life. Death - Make a 5/5 golem token and "
     "gain 1 coin.",
     1, false},
    {162, "Grave Beetle", 3, true, "bug", 6, 6, "fly;dash",
     "Attack - Dig 3, then for each unit in your graveyard, put 1 power "
     "counter on this unit and gain 1 life.",
     1, false},
    {163, "Chrono Mage", 3, true, "mage", 7, 9, "",
     "Enter - Choose up to 1 unit and up to 1 support from your graveyard and "
     "add them to hand. Ability - Discard 1 card to make a 10/10 golem token.",
     1, false},
    {164, "Fortune Merchant", 3, true, "merchant", 5, 8, "trade",
     "Ability - Discard any number of cards to gain twice that many coins. "
     "Closure - Draw cards until you have 3 cards in hand.",
     1, false},
    {165, "Infernal Broodlord", 3, true, "demon", 11, 8, "dash",
     "Attack - Make a 3/1 demon token with dash. Closure - Discard 1 card and "
     "lose 3 life.",
     1, false},
    {166, "Pack Alpha", 3, true, "beast", 9, 9, "overrun",
     "Passive - Other ally beasts have overrun and gain +3/+3 while this unit "
     "is on the field. Attack - Make a 3/3 beast token.",
     1, false},
    {167, "Berserker Champion", 3, true, "berserker", 15, 5, "dash;overrun",
     "Enter - Discard your hand and dig 5, then you lose 5 life.", 1, false},
    {168, "Arcane Deployer", 3, true, "mage", 8, 10, "",
     "Ability - Deploy a unit from your hand, then put 4 power counters on it.",
     1, false},
    // Cost 4 rare
    {169, "Supreme Conjurer", 4, true, "mage", 10, 12, "",
     "Ability - Deploy a unit from your graveyard, then put 5 power counters "
     "on it.",
     1, false},
    {170, "Infernal Overlord", 4, true, "demon", 11, 11, "fly;overrun",
     "Passive - Other ally units gain +6/+6 while this unit is on the field. "
     "Closure - Sacrifice another ally or lose 11 life.",
     1, false},
    {171, "Celestial Paragon", 4, true, "angel", 12, 12,
     "fly;overrun;dash;tenacity",
     "Passive - Other ally units gain fly, overrun, dash and tenacity while "
     "this unit is on the field.",
     1, false},
    // Cost 5 rare
    {172, "Apex Predator", 5, true, "beast", 12, 14, "dash;overrun",
     "Enter - Make a 6/6 beast token. Attack - Put 6 power counters on each "
     "other ally.",
     1, false},
    {173, "Pit Lord", 5, true, "demon", 13, 13, "fly;overrun",
     "Enter - Destroy an enemy. Harvest - Your opponent sacrifices a unit.", 1,
     false},
    // Cost 6 rare
    {174, "Archangel of Wrath", 6, true, "angel", 15, 15, "fly;dash",
     "Enter - Deal 7 damage to each enemy. Attack - Make a 7/7 Angel token "
     "with fly, then gain 7 life.",
     1, false},
    // ══════════════════════ RARE SUPPORTS (id 175-192) ═══════════════════════
    {175, "Mind Shackle", 2, false, "", 0, 0, "",
     "Gain control over an enemy with atk 4 or less.", 1, false},
    {176, "Power Surge", 2, false, "", 0, 0, "",
     "Put 5 power counter on an ally, then destroy an enemy with less atk than "
     "that ally.",
     1, false},
    {177, "Arcane Burst", 2, false, "", 0, 0, "",
     "Draw 2 cards, then discard 1 card, then if the discarded card was a "
     "unit, deal damage equal to its atk.",
     1, false},
    {178, "Celestial Army", 2, false, "", 0, 0, "",
     "For each ally, make a 7/7 angel token with fly.", 1, false},
    {179, "War Cry", 2, false, "", 0, 0, "",
     "Allies deal double combat damage this turn.", 1, false},
    {180, "Graveyard Feast", 3, false, "", 0, 0, "",
     "Add up to 3 cards from your graveyard to hand.", 1, false},
    {181, "Domination", 3, false, "", 0, 0, "", "Gain control over an enemy.",
     1, false},
    {182, "Soul Recall", 3, false, "", 0, 0, "",
     "Deploy a unit from your graveyard.", 1, false},
    {183, "Cataclysm", 3, false, "", 0, 0, "", "Destroy all units.", 1, false},
    {184, "Cerebral Storm", 4, false, "", 0, 0, "",
     "Draw 3 cards, then deal damage to each enemy equal to the number of "
     "cards in your hand.",
     1, false},
    {185, "Mass Revival", 4, false, "", 0, 0, "",
     "Deploy all units with atk 4 or less from your graveyard.", 1, false},
    {186, "Shadow Summon", 4, false, "", 0, 0, "",
     "Deploy a unit from your hand.", 1, false},
    {187, "Triple Punishment", 4, false, "", 0, 0, "",
     "Your opponent discards 2 cards, sacrifices 2 units and loses 4 life.", 1,
     false},
    {188, "Bug Swarm", 4, false, "", 0, 0, "",
     "For each card in your hand, make a 2/2 bug token, then for each card in "
     "your hand, put 2 power counters on each ally bug.",
     1, false},
    {189, "Annihilation", 5, false, "", 0, 0, "", "Destroy all enemies.", 1,
     false},
    {190, "Grand Bargain", 5, false, "", 0, 0, "",
     "Draw 3 cards and gain 6 life, then make a 12/12 beast token.", 1, false},
    {191, "Mass Subjugation", 5, false, "", 0, 0, "",
     "Gain control over each enemy with atk 4 or less, then put 4 power "
     "counters on each of them.",
     1, false},
    {192, "Rise from Ashes", 6, false, "", 0, 0, "",
     "Deploy each unit from your graveyard.", 1, false},
    {193, "Nature's Army", 6, false, "", 0, 0, "",
     "Make six 6/6 beast tokens with overrun.", 1, false},
};
static const int NUM_ALL_CARDS = sizeof(ALL_CARDS) / sizeof(ALL_CARDS[0]);

static bool CardHasKeyword(const CardDef &cd, const char *kw) {
  if (!cd.keywords || !cd.keywords[0])
    return false;
  return strstr(cd.keywords, kw) != nullptr;
}

// ── Match State ─────────────────────────────────────────────────────────────
static constexpr int MAX_HAND = 10;
static constexpr int MAX_FIELD = 8;
static constexpr int MAX_DECK = 40;
static constexpr int MAX_GRAVE = 60;

struct FieldUnit {
  int cardId;             // references ALL_CARDS by id
  int curAtk, curDef;     // current stats (may be modified by counters)
  int bonusAtk, bonusDef; // For temporary effects
  bool isDefender;
  bool canActivate;  // false on turn deployed (unless Dash)
  bool activated;    // already attacked/used ability this turn
  int powerCounters; // each adds +1/+1
  int weakCounters;  // each adds -1/-1
  bool alive;
  int goldCounters; // for Golden Golem's gold counter ability
};

struct MatchPlayer {
  int life;
  int coins;
  int hand[MAX_HAND]; // card IDs
  int handSize;
  int deck[MAX_DECK]; // card IDs, deck[deckTop-1] is top
  int deckSize;
  FieldUnit field[MAX_FIELD];
  int fieldSize;
  int grave[MAX_GRAVE];
  int graveSize;
  bool isAI;
};

enum MatchPhase {
  PHASE_COLLECT,
  PHASE_DEVELOP,
  PHASE_ACTIVATE,
  PHASE_END,
  PHASE_GAME_OVER
};
enum MatchAction {
  ACT_NONE,
  ACT_SELECT_HAND,
  ACT_SELECT_FIELD,
  ACT_SELECT_TARGET,
  ACT_CONFIRM
};

struct GameMatch {
  MatchPlayer players[2]; // 0=human, 1=AI
  int turn;               // whose turn (0 or 1)
  MatchPhase phase;
  int turnNumber;
  bool active;
  bool playerWon;
  int selectedHandIdx; // for human input
  int selectedFieldIdx;
  int targetFieldIdx;
  MatchAction pendingAction;
  int challengedNPC; // which NPC started this match
  float messageTimer;
  char message[128];
  // For support card targeting
  int pendingSupportCard; // card id of support being resolved, -1 if none
  bool needsTarget;
  // Turn-based effect flags
  bool warCryActive; // War Cry: allies deal double combat damage this turn
  int ratsAttackedThisTurn; // for Rat Commander Closure
};

static GameMatch g_match;

// ── Player Collection & Shop ────────────────────────────────────────────────
static constexpr int MAX_COLLECTION = 600;
static int g_collection[MAX_COLLECTION]; // card IDs owned
static int g_collectionSize = 0;
static int g_playerDeck[MAX_DECK]; // current deck (card IDs)
static int g_playerDeckSize = 0;
static int g_playerCoins = 0; // persistent coins for shop
static bool g_hasStarterDeck = false;
static int g_shopScroll = 0;

// ── Menu System ─────────────────────────────────────────────────────────────
static bool g_menuOpen = false;
static bool g_npcDialogOpen = false;
static int g_targetNPC = -1;
static int g_dialogSelection = 0;

// ── Dialogue state machine ──────────────────────────────────────────────────
enum DialogPhase { DIALOG_MENU = 0, DIALOG_TALK };
static DialogPhase g_dialogPhase = DIALOG_MENU;
static const char *g_dialogText = nullptr; // current talk text to display

// Card-game lore lines per NPC (index matches g_npcs[])
static const char *g_npcLore[4][3] = {
    { // 0: Fatima
        "The dunes hide many treasures, but none\nas rare as an Epic card from the Nomad faction.",
        "My grandmother once held a Legendary Sovereign\ncard. She traded it for safe passage across the Sands.",
        "Coins flow like sand here. Spend them wisely\nat the bazaar, or save for tournament entry."
    },
    { // 1: Scholar
        "The ancient texts speak of five factions\u2014\neach with a unique strategy to master.",
        "A wise duelist knows that card synergy\nmatters more than raw rarity.",
        "I have catalogued every Common and Uncommon\ncard. The Epics... those I only dream of."
    },
    { // 2: Yara (Merchant)
        "Business is good when the caravans arrive.\nI stock fresh packs every cycle.",
        "Rare cards fetch a high price, but the real\nvalue is in building a balanced deck.",
        "The Merchant faction rewards patience.\nHoard your Coins and strike at the right moment."
    },
    { // 3: Kai
        "I craft custom card sleeves in my workshop.\nProtect your deck from the desert wind!",
        "Rumor has it a hidden faction exists\u2014\nonly unlocked by winning ten duels in a row.",
        "The tournament grounds open at dusk.\nBring your strongest twenty cards."
    }
};
enum MenuTab {
  TAB_COLLECTION = 0,
  TAB_DECKS,
  TAB_SAVE,
  TAB_SETTINGS,
  TAB_COUNT
};
static MenuTab g_menuTab = TAB_COLLECTION;
static int g_collScroll = 0;      // collection browser scroll
static int g_selectedCardId = -1; // selected card in collection view
static int g_deckScroll = 0;      // deckbuilder deck list scroll

// Card copies tracking
struct CardCopy {
  int cardId;
  int count;
};
static CardCopy g_cardCopies[80]; // aggregated collection
static int g_numCardCopies = 0;

static void RebuildCardCopies() {
  g_numCardCopies = 0;
  for (int i = 0; i < g_collectionSize; i++) {
    int cid = g_collection[i];
    bool found = false;
    for (int j = 0; j < g_numCardCopies; j++) {
      if (g_cardCopies[j].cardId == cid) {
        g_cardCopies[j].count++;
        found = true;
        break;
      }
    }
    if (!found && g_numCardCopies < 80) {
      g_cardCopies[g_numCardCopies++] = {cid, 1};
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// MARKET CONTROLLER — Dynamic Economy Engine
// ═══════════════════════════════════════════════════════════════════════════════
enum CardRarity { RARITY_COMMON = 0, RARITY_RARE, RARITY_EPIC };

static CardRarity GetCardRarity(int cardId) {
  if (cardId <= 0 || cardId > NUM_ALL_CARDS)
    return RARITY_COMMON;
  const CardDef &cd = ALL_CARDS[cardId - 1];
  if (cd.rarity == 1)
    return RARITY_RARE;
  if (cd.isUnique)
    return RARITY_EPIC; // unique commons are epic-tier pricing
  return RARITY_COMMON;
}

struct MarketController {
  float basePrice[NUM_ALL_CARDS + 1];      // base price per card ID (1-indexed)
  float currentPrice[NUM_ALL_CARDS + 1];   // live price with all modifiers
  float volatility[NUM_ALL_CARDS + 1];     // accumulated % modifier
  int npcCopies[NUM_ALL_CARDS + 1];        // total copies held by all NPCs
  bool influencerBoost[NUM_ALL_CARDS + 1]; // active influencer multiplier
  float influencerMult[NUM_ALL_CARDS + 1]; // 1.5x-5x if active

  void Init() {
    for (int i = 1; i <= NUM_ALL_CARDS; i++) {
      CardRarity r = GetCardRarity(i);
      if (r == RARITY_COMMON)
        basePrice[i] = (float)(1 + ALL_CARDS[i - 1].cost * 1);
      else if (r == RARITY_RARE)
        basePrice[i] = (float)(10 + ALL_CARDS[i - 1].cost * 20);
      else
        basePrice[i] = (float)(100 + ALL_CARDS[i - 1].cost * 100);
      volatility[i] = 1.0f;
      npcCopies[i] = 10; // NPCs start with 10 copies of each card
      influencerBoost[i] = false;
      influencerMult[i] = 1.0f;
    }
    RecalcPrices();
  }

  void RecalcPrices() {
    for (int i = 1; i <= NUM_ALL_CARDS; i++) {
      float p = basePrice[i] * volatility[i];
      // Monopoly trigger: if no NPC copies and player has copies
      int playerCopies = 0;
      for (int c = 0; c < g_collectionSize; c++)
        if (g_collection[c] == i)
          playerCopies++;
      if (npcCopies[i] == 0 && playerCopies > 0)
        p *= 10.0f;
      // Influencer boost
      if (influencerBoost[i])
        p *= influencerMult[i];
      // Day/night modifier applied externally
      currentPrice[i] = p;
      if (currentPrice[i] < 1.0f)
        currentPrice[i] = 1.0f;
    }
  }

  void OnPackOpened() {
    // Every pack opened: decrease all prices by 1%
    for (int i = 1; i <= NUM_ALL_CARDS; i++)
      volatility[i] *= 0.99f;
    RecalcPrices();
  }

  void OnCardBought(int cardId) {
    if (cardId >= 1 && cardId <= NUM_ALL_CARDS) {
      volatility[cardId] *= 1.01f; // 1% increase
      if (npcCopies[cardId] > 0)
        npcCopies[cardId]--;
      RecalcPrices();
    }
  }

  void OnCardSold(int cardId) {
    if (cardId >= 1 && cardId <= NUM_ALL_CARDS) {
      volatility[cardId] *= 0.99f; // 1% decrease
      npcCopies[cardId]++;
      RecalcPrices();
    }
  }

  void OnTournamentEnd(const int *winDeckIds, int winCount,
                       const int *loseDeckIds, int loseCount) {
    // Winners: +5% price increase
    for (int i = 0; i < winCount; i++) {
      int cid = winDeckIds[i];
      if (cid >= 1 && cid <= NUM_ALL_CARDS)
        volatility[cid] *= 1.05f;
    }
    // Losers: -3% price decrease
    for (int i = 0; i < loseCount; i++) {
      int cid = loseDeckIds[i];
      if (cid >= 1 && cid <= NUM_ALL_CARDS)
        volatility[cid] *= 0.97f;
    }
    RecalcPrices();
  }

  void ApplyInfluencer(int cardId, float mult) {
    if (cardId >= 1 && cardId <= NUM_ALL_CARDS) {
      influencerBoost[cardId] = true;
      influencerMult[cardId] = Clamp(mult, 1.5f, 5.0f);
      RecalcPrices();
    }
  }

  int GetBuyPrice(int cardId, float dayMod = 1.0f) {
    if (cardId < 1 || cardId > NUM_ALL_CARDS)
      return 0;
    return (int)(currentPrice[cardId] * dayMod + 0.5f);
  }

  int GetSellPrice(int cardId, float dayMod = 1.0f,
                   float sellerCertMult = 1.0f) {
    if (cardId < 1 || cardId > NUM_ALL_CARDS)
      return 0;
    return (int)(currentPrice[cardId] * 0.5f * dayMod * sellerCertMult + 0.5f);
  }
};
static MarketController g_market;

// ═══════════════════════════════════════════════════════════════════════════════
// TOURNAMENT & LEAGUE STRUCTURE
// ═══════════════════════════════════════════════════════════════════════════════
enum CityArchetype {
  CITY_AGGRO = 0,
  CITY_CONTROL,
  CITY_COMBO,
  CITY_MIDRANGE,
  CITY_TEMPO,
  CITY_COUNT
};
enum LeagueTier {
  LEAGUE_BRONZE = 0,
  LEAGUE_SILVER,
  LEAGUE_GOLD,
  LEAGUE_PLATINUM,
  LEAGUE_DIAMOND,
  LEAGUE_COUNT
};

struct TournamentManager {
  int currentCity;     // which city (0-4)
  int currentLeague;   // which tier (0-4)
  int roundsWon;       // 0-3 per league
  int roundsPerLeague; // 3
  bool cityChampionDefeated[CITY_COUNT];
  bool capitalUnlocked; // all 5 champions beaten
  int totalTournamentsWon;

  void Init() {
    currentCity = 0;
    currentLeague = 0;
    roundsWon = 0;
    roundsPerLeague = 3;
    memset(cityChampionDefeated, 0, sizeof(cityChampionDefeated));
    capitalUnlocked = false;
    totalTournamentsWon = 0;
  }

  int GetPrizeCoins() {
    // Bronze=20, Silver=50, Gold=100, Platinum=250, Diamond=500
    const int prizes[] = {20, 50, 100, 250, 500};
    return prizes[currentLeague];
  }

  float GetDifficultyMult() {
    // Scale NPC deck strength: 1.0 (Bronze) to 2.5 (Diamond)
    return 1.0f + currentLeague * 0.375f;
  }

  bool AdvanceRound(bool won, int *winDeck, int winCount, int *loseDeck,
                    int loseCount) {
    if (won) {
      roundsWon++;
      g_playerCoins += GetPrizeCoins();
      // Apply tournament price changes
      g_market.OnTournamentEnd(winDeck, winCount, loseDeck, loseCount);

      if (roundsWon >= roundsPerLeague) {
        roundsWon = 0;
        if (currentLeague == LEAGUE_DIAMOND) {
          // Defeated city champion
          cityChampionDefeated[currentCity] = true;
          totalTournamentsWon++;
          // Check if all cities cleared → unlock capital
          capitalUnlocked = true;
          for (int i = 0; i < CITY_COUNT; i++)
            if (!cityChampionDefeated[i])
              capitalUnlocked = false;
          currentLeague = 0;
          return true; // city complete
        }
        currentLeague++;
      }
    }
    return false;
  }

  const char *GetCityName() {
    const char *names[] = {"Embervale (Aggro)", "Tideguard (Control)",
                           "Arcana (Combo)", "Ironhearth (Midrange)",
                           "Zephyra (Tempo)"};
    return names[currentCity % CITY_COUNT];
  }
  const char *GetLeagueName() {
    const char *names[] = {"Bronze", "Silver", "Gold", "Platinum", "Diamond"};
    return names[currentLeague % LEAGUE_COUNT];
  }
};
static TournamentManager g_tournament;

// ═══════════════════════════════════════════════════════════════════════════════
// DAY/NIGHT CYCLE — 4 states with shop price modifiers
// ═══════════════════════════════════════════════════════════════════════════════
enum TimeOfDay { TIME_MORNING = 0, TIME_DAY, TIME_EVENING, TIME_NIGHT };
struct WorldClock {
  float elapsed;     // total game seconds
  float cycleLength; // seconds per full day cycle
  TimeOfDay current;

  void Init() {
    elapsed = 0;
    cycleLength = 240.0f;
    current = TIME_MORNING;
  } // 4 min cycle

  void Update(float dt) {
    elapsed += dt;
    float phase = fmodf(elapsed, cycleLength) / cycleLength;
    if (phase < 0.25f)
      current = TIME_MORNING;
    else if (phase < 0.5f)
      current = TIME_DAY;
    else if (phase < 0.75f)
      current = TIME_EVENING;
    else
      current = TIME_NIGHT;
  }

  float GetShopMod() {
    switch (current) {
    case TIME_MORNING:
      return 1.07f; // +7%
    case TIME_DAY:
      return 1.00f;
    case TIME_EVENING:
      return 1.03f; // +3%
    case TIME_NIGHT:
      return 1.05f; // +5%
    }
    return 1.0f;
  }

  Color GetAmbientTint() {
    switch (current) {
    case TIME_MORNING:
      return {255, 240, 210, 255}; // warm gold
    case TIME_DAY:
      return {255, 255, 245, 255}; // neutral bright
    case TIME_EVENING:
      return {255, 200, 150, 255}; // orange sunset
    case TIME_NIGHT:
      return {140, 150, 200, 255}; // cool blue
    }
    return WHITE;
  }

  const char *GetName() {
    const char *names[] = {"Morning", "Day", "Evening", "Night"};
    return names[current];
  }
};
static WorldClock g_worldClock;

// ═══════════════════════════════════════════════════════════════════════════════
// INVENTORY SYSTEM — Passive buff items
// ═══════════════════════════════════════════════════════════════════════════════
enum ItemId {
  ITEM_KINGDOM_MAP = 0,
  ITEM_FAIR_SCALE,
  ITEM_SELLERS_CERT,
  ITEM_LUCKY_CHARM,
  ITEM_TRADERS_BADGE,
  ITEM_COUNT
};
struct InventoryItem {
  ItemId id;
  const char *name;
  const char *desc;
  bool owned;
};
struct InventorySystem {
  InventoryItem items[ITEM_COUNT];

  void Init() {
    items[ITEM_KINGDOM_MAP] = {ITEM_KINGDOM_MAP, "Kingdom Map",
                               "Shows the world map.", true};
    items[ITEM_FAIR_SCALE] = {ITEM_FAIR_SCALE, "Fair Scale",
                              "Doubles rarity roll in pack opening.", false};
    items[ITEM_SELLERS_CERT] = {ITEM_SELLERS_CERT, "Seller's Certificate",
                                "1.2x sell price multiplier.", false};
    items[ITEM_LUCKY_CHARM] = {ITEM_LUCKY_CHARM, "Lucky Charm",
                               "+10% coin bonus from wins.", false};
    items[ITEM_TRADERS_BADGE] = {ITEM_TRADERS_BADGE, "Trader's Badge",
                                 "Unlock rare cards in shops.", false};
  }

  bool Has(ItemId id) { return items[id].owned; }
  void Give(ItemId id) { items[id].owned = true; }

  float GetSellMult() { return Has(ITEM_SELLERS_CERT) ? 1.2f : 1.0f; }
  float GetWinCoinMult() { return Has(ITEM_LUCKY_CHARM) ? 1.1f : 1.0f; }
  bool HasFairScale() { return Has(ITEM_FAIR_SCALE); }
};
static InventorySystem g_inventory;

// ═══════════════════════════════════════════════════════════════════════════════
// ONBOARDING STATE — Starting script (declared early for SaveData)
// ═══════════════════════════════════════════════════════════════════════════════
static bool g_onboardingDone = false;
static bool g_grandpaTutorialDone = false;
static int g_onboardingStep =
    0; // 0=intro, 1=grandpa dialog, 2=tutorial battle, 3=done

// ═══════════════════════════════════════════════════════════════════════════════
// SAVE/LOAD SYSTEM — Binary serialization to savegame.dat
// ═══════════════════════════════════════════════════════════════════════════════
static const unsigned int SAVE_MAGIC = 0x534F5647; // "SOVG"
static const int SAVE_VERSION = 1;

struct SaveData {
  unsigned int magic;
  int version;
  // Player state
  int gridX, gridY;
  int dir;
  int coins;
  // Collection & deck
  int collectionSize;
  int collection[MAX_COLLECTION];
  int deckSize;
  int deck[MAX_DECK];
  bool hasStarterDeck;
  // Tournament
  int tournCity, tournLeague, tournRoundsWon, tournTotal;
  bool cityChampDefeated[5];
  bool capitalUnlocked;
  // World clock
  float clockElapsed;
  // Inventory
  bool itemsOwned[5]; // ITEM_COUNT
  // Market volatility snapshot
  float marketVolatility[200]; // up to NUM_ALL_CARDS
  int marketNpcCopies[200];
  // Onboarding
  bool onboardingDone;
  bool grandpaTutorialDone;
};

static bool SaveGame() {
  SaveData sd = {};
  sd.magic = SAVE_MAGIC;
  sd.version = SAVE_VERSION;
  sd.gridX = g_player.gridX;
  sd.gridY = g_player.gridY;
  sd.dir = (int)g_player.dir;
  sd.coins = g_playerCoins;
  sd.collectionSize = g_collectionSize;
  for (int i = 0; i < g_collectionSize; i++)
    sd.collection[i] = g_collection[i];
  sd.deckSize = g_playerDeckSize;
  for (int i = 0; i < g_playerDeckSize; i++)
    sd.deck[i] = g_playerDeck[i];
  sd.hasStarterDeck = g_hasStarterDeck;
  sd.tournCity = g_tournament.currentCity;
  sd.tournLeague = g_tournament.currentLeague;
  sd.tournRoundsWon = g_tournament.roundsWon;
  sd.tournTotal = g_tournament.totalTournamentsWon;
  for (int i = 0; i < 5; i++)
    sd.cityChampDefeated[i] = g_tournament.cityChampionDefeated[i];
  sd.capitalUnlocked = g_tournament.capitalUnlocked;
  sd.clockElapsed = g_worldClock.elapsed;
  for (int i = 0; i < ITEM_COUNT; i++)
    sd.itemsOwned[i] = g_inventory.items[i].owned;
  int n = NUM_ALL_CARDS < 200 ? NUM_ALL_CARDS : 200;
  for (int i = 0; i < n; i++) {
    sd.marketVolatility[i] = g_market.volatility[i + 1];
    sd.marketNpcCopies[i] = g_market.npcCopies[i + 1];
  }
  sd.onboardingDone = g_onboardingDone;
  sd.grandpaTutorialDone = g_grandpaTutorialDone;

  FILE *f = fopen("savegame.dat", "wb");
  if (!f)
    return false;
  fwrite(&sd, sizeof(SaveData), 1, f);
  fclose(f);
  return true;
}

static bool LoadGame() {
  FILE *f = fopen("savegame.dat", "rb");
  if (!f)
    return false;
  SaveData sd = {};
  size_t read = fread(&sd, sizeof(SaveData), 1, f);
  fclose(f);
  if (read != 1 || sd.magic != SAVE_MAGIC || sd.version != SAVE_VERSION)
    return false;

  g_player.gridX = sd.gridX;
  g_player.gridY = sd.gridY;
  g_player.posX = (float)sd.gridX;
  g_player.posZ = (float)sd.gridY;
  g_player.velX = g_player.velZ = 0;
  g_player.dir = (Dir)sd.dir;
  g_player.moving = false;
  g_playerCoins = sd.coins;
  g_collectionSize = sd.collectionSize;
  for (int i = 0; i < sd.collectionSize; i++)
    g_collection[i] = sd.collection[i];
  g_playerDeckSize = sd.deckSize;
  for (int i = 0; i < sd.deckSize; i++)
    g_playerDeck[i] = sd.deck[i];
  g_hasStarterDeck = sd.hasStarterDeck;
  g_tournament.currentCity = sd.tournCity;
  g_tournament.currentLeague = sd.tournLeague;
  g_tournament.roundsWon = sd.tournRoundsWon;
  g_tournament.totalTournamentsWon = sd.tournTotal;
  for (int i = 0; i < 5; i++)
    g_tournament.cityChampionDefeated[i] = sd.cityChampDefeated[i];
  g_tournament.capitalUnlocked = sd.capitalUnlocked;
  g_worldClock.elapsed = sd.clockElapsed;
  for (int i = 0; i < ITEM_COUNT; i++)
    g_inventory.items[i].owned = sd.itemsOwned[i];
  int n = NUM_ALL_CARDS < 200 ? NUM_ALL_CARDS : 200;
  for (int i = 0; i < n; i++) {
    g_market.volatility[i + 1] = sd.marketVolatility[i];
    g_market.npcCopies[i + 1] = sd.marketNpcCopies[i];
  }
  g_market.RecalcPrices();
  g_onboardingDone = sd.onboardingDone;
  g_grandpaTutorialDone = sd.grandpaTutorialDone;
  g_scene = SCENE_OVERWORLD;
  return true;
}

static bool g_saveNotification = false;
static float g_saveNotifyTimer = 0;

// ── Starter Deck ────────────────────────────────────────────────────────────
static void GiveStarterDeck() {
  // 30 cards: mix of cost 0-2 (new IDs from full CSV)
  const int starterIds[] = {
      1, 2, 3, 4, 5, 6, 7, 9, 10, 11,    // 10 cheap units (cost 1)
      12, 13, 14, 15, 16, 17, 18, 20,    // 8 more cost-1 units
      108, 108, 113, 111, 112, 116, 116, // 7 support cards (Arrow x2, Luck,
                                         // Grow, Obsession, Fire Bolt x2)
      29, 31, 41, 43, 14                 // 5 more units
  };
  g_playerDeckSize = 30;
  for (int i = 0; i < 30; i++)
    g_playerDeck[i] = starterIds[i];
  // Also add to collection
  for (int i = 0; i < 30; i++) {
    if (g_collectionSize < MAX_COLLECTION)
      g_collection[g_collectionSize++] = starterIds[i];
  }
  g_hasStarterDeck = true;
}

// ── Deck Building for NPC ───────────────────────────────────────────────────
static void BuildNPCDeck(int npcIdx, int *deck, int &deckSize) {
  deckSize = 30;
  switch (npcIdx) {
  case 0: { // Fatima: Rat/Beast aggro
    const int ids[] = {2,   2,   16,  16,  17,  17,  18,  49,  49, 12,
                       12,  42,  42,  41,  41,  65,  5,   5,   6,  6,
                       108, 108, 116, 116, 111, 111, 112, 114, 13, 14};
    for (int i = 0; i < 30; i++)
      deck[i] = ids[i];
  } break;
  case 1: { // Sage Karim: Mage/Spirit control
    const int ids[] = {7,   7,   29,  29,  31,  31,  58,  58,  77,  96,
                       9,   9,   11,  11,  40,  40,  64,  64,  116, 116,
                       119, 119, 123, 123, 120, 120, 114, 114, 108, 111};
    for (int i = 0; i < 30; i++)
      deck[i] = ids[i];
  } break;
  case 2: { // Merchant Yara: Plant/Merchant value
    const int ids[] = {3,   3,   20,  20,  86,  98,  10,  10,  113, 113,
                       113, 136, 120, 120, 122, 122, 111, 111, 41,  41,
                       4,   4,   52,  74,  108, 108, 114, 114, 115, 115};
    for (int i = 0; i < 30; i++)
      deck[i] = ids[i];
  } break;
  case 3: { // Chief Omari: Demon/Zombie aggro
    const int ids[] = {11,  11,  38,  38,  45,  45,  46,  46,  70,  106,
                       27,  27,  55,  76,  6,   6,   14,  14,  42,  42,
                       108, 108, 116, 116, 112, 112, 123, 126, 127, 129};
    for (int i = 0; i < 30; i++)
      deck[i] = ids[i];
  } break;
  }
}

// ── Shuffle Array ───────────────────────────────────────────────────────────
static void ShuffleDeck(int *arr, int n) {
  for (int i = n - 1; i > 0; i--) {
    int j = rand() % (i + 1);
    int tmp = arr[i];
    arr[i] = arr[j];
    arr[j] = tmp;
  }
}

// ── Find CardDef by ID ──────────────────────────────────────────────────────
static const CardDef &GetCard(int id) {
  for (int i = 0; i < NUM_ALL_CARDS; i++)
    if (ALL_CARDS[i].id == id)
      return ALL_CARDS[i];
  return ALL_CARDS[0]; // fallback
}

// ── Initialize a Match ──────────────────────────────────────────────────────
static void StartMatch(int npcIdx) {
  srand((unsigned)time(nullptr));
  memset(&g_match, 0, sizeof(g_match));
  g_match.active = true;
  g_match.challengedNPC = npcIdx;
  g_match.turn = 0;
  g_match.turnNumber = 1;
  g_match.phase = PHASE_COLLECT;
  g_match.selectedHandIdx = -1;
  g_match.selectedFieldIdx = -1;
  g_match.targetFieldIdx = -1;
  g_match.pendingAction = ACT_NONE;
  g_match.pendingSupportCard = -1;
  g_match.needsTarget = false;
  snprintf(g_match.message, 128, "Duel started!");
  g_match.messageTimer = 2.0f;

  for (int p = 0; p < 2; p++) {
    MatchPlayer &mp = g_match.players[p];
    mp.life = 20;
    mp.coins = 0;
    mp.handSize = 0;
    mp.fieldSize = 0;
    mp.graveSize = 0;
    mp.isAI = (p == 1);

    if (p == 0) {
      // Player deck
      mp.deckSize = g_playerDeckSize;
      for (int i = 0; i < mp.deckSize; i++)
        mp.deck[i] = g_playerDeck[i];
    } else {
      // NPC deck
      BuildNPCDeck(npcIdx, mp.deck, mp.deckSize);
    }
    ShuffleDeck(mp.deck, mp.deckSize);

    // Draw starting hand of 5
    for (int i = 0; i < 5 && mp.deckSize > 0; i++) {
      mp.hand[mp.handSize++] = mp.deck[--mp.deckSize];
    }
  }
}

// ── Draw a card from deck to hand ───────────────────────────────────────────
static bool MatchDrawCard(MatchPlayer &mp) {
  if (mp.deckSize <= 0)
    return false;
  if (mp.handSize >= MAX_HAND)
    return false; // hand full, card lost
  mp.hand[mp.handSize++] = mp.deck[--mp.deckSize];
  return true;
}

// ── Discard from hand ───────────────────────────────────────────────────────
static void MatchDiscard(MatchPlayer &mp, int handIdx) {
  if (handIdx < 0 || handIdx >= mp.handSize)
    return;
  if (mp.graveSize < MAX_GRAVE)
    mp.grave[mp.graveSize++] = mp.hand[handIdx];
  for (int i = handIdx; i < mp.handSize - 1; i++)
    mp.hand[i] = mp.hand[i + 1];
  mp.handSize--;
}

// ── Deploy a unit from hand to field ────────────────────────────────────────
static bool MatchDeployUnit(GameMatch &m, int playerIdx, int handIdx) {
  MatchPlayer &mp = m.players[playerIdx];
  if (handIdx < 0 || handIdx >= mp.handSize)
    return false;
  int cardId = mp.hand[handIdx];
  const CardDef &cd = GetCard(cardId);
  if (!cd.isUnit)
    return false;
  if (mp.coins < cd.cost)
    return false;
  if (mp.fieldSize >= MAX_FIELD)
    return false;

  mp.coins -= cd.cost;
  FieldUnit &fu = mp.field[mp.fieldSize++];
  fu.cardId = cardId;
  fu.curAtk = cd.atk;
  fu.curDef = cd.def;
  fu.bonusAtk = 0;
  fu.bonusDef = 0;
  fu.isDefender = false;
  fu.canActivate = CardHasKeyword(cd, "dash");
  fu.activated = false;
  fu.powerCounters = 0;
  fu.weakCounters = 0;
  fu.alive = true;
  fu.goldCounters = 0;

  // Remove from hand
  for (int i = handIdx; i < mp.handSize - 1; i++)
    mp.hand[i] = mp.hand[i + 1];
  mp.handSize--;

  // Simple Enter effects
  MatchPlayer &opp = m.players[1 - playerIdx];
  if (strstr(cd.effect, "Enter") && strstr(cd.effect, "Gain") &&
      strstr(cd.effect, "life")) {
    // Extract number
    const char *p = strstr(cd.effect, "Gain");
    if (p) {
      int val = atoi(p + 5);
      mp.life += val;
    }
  }
  if (strstr(cd.effect, "Enter") && strstr(cd.effect, "Deal") &&
      strstr(cd.effect, "damage")) {
    const char *p = strstr(cd.effect, "Deal");
    if (p) {
      int val = atoi(p + 5);
      // Deal damage to weakest enemy unit if possible
      if (opp.fieldSize > 0) {
        int weakest = 0;
        for (int i = 1; i < opp.fieldSize; i++)
          if (opp.field[i].curDef + opp.field[i].powerCounters -
                  opp.field[i].weakCounters <
              opp.field[weakest].curDef + opp.field[weakest].powerCounters -
                  opp.field[weakest].weakCounters)
            weakest = i;
        opp.field[weakest].curDef -= val;
        if (opp.field[weakest].curDef + opp.field[weakest].powerCounters -
                opp.field[weakest].weakCounters <=
            0)
          opp.field[weakest].alive = false;
      } else {
        opp.life -= val;
      }
    }
  }
  if (strstr(cd.effect, "Enter") && strstr(cd.effect, "Opponent loses") &&
      strstr(cd.effect, "life")) {
    const char *p = strstr(cd.effect, "loses");
    if (p) {
      int val = atoi(p + 6);
      opp.life -= val;
    }
  }
  if (strstr(cd.effect, "Enter") && strstr(cd.effect, "Opponent discards")) {
    if (opp.handSize > 0)
      MatchDiscard(opp, rand() % opp.handSize);
  }
  if (strstr(cd.effect, "Enter") && strstr(cd.effect, "Draw")) {
    const char *p = strstr(cd.effect, "Draw");
    if (p) {
      int val = atoi(p + 5);
      for (int i = 0; i < val; i++)
        MatchDrawCard(mp);
    }
  }
  if (strstr(cd.effect, "Enter") && strstr(cd.effect, "power counters on")) {
    const char *p = strstr(cd.effect, "Put");
    if (p) {
      int val = atoi(p + 4);
      if (strstr(cd.effect, "each other ally")) {
        for (int i = 0; i < mp.fieldSize; i++)
          if (i != mp.fieldSize - 1)
            mp.field[i].powerCounters += val;
      } else if (strstr(cd.effect, "another ally")) {
        // Buff strongest other ally
        if (mp.fieldSize > 1) {
          int best = 0;
          if (best == mp.fieldSize - 1)
            best = 1;
          for (int i = 0; i < mp.fieldSize; i++) {
            if (i == mp.fieldSize - 1)
              continue;
            if (mp.field[i].curAtk + mp.field[i].powerCounters >
                mp.field[best].curAtk + mp.field[best].powerCounters)
              best = i;
          }
          mp.field[best].powerCounters += val;
        }
      }
    }
  }
  if (strstr(cd.effect, "Enter") && strstr(cd.effect, "weak counter")) {
    const char *p = strstr(cd.effect, "Put");
    if (p) {
      int val = atoi(p + 4);
      if (strstr(cd.effect, "each enemy")) {
        for (int i = 0; i < opp.fieldSize; i++)
          opp.field[i].weakCounters += val;
      } else if (strstr(cd.effect, "an enemy") && opp.fieldSize > 0) {
        // Weakest enemy
        int weakest = 0;
        for (int i = 1; i < opp.fieldSize; i++)
          if (opp.field[i].curDef < opp.field[weakest].curDef)
            weakest = i;
        opp.field[weakest].weakCounters += val;
      }
    }
  }
  if (strstr(cd.effect, "Enter") && strstr(cd.effect, "Each player loses")) {
    const char *p = strstr(cd.effect, "loses");
    if (p) {
      int val = atoi(p + 6);
      mp.life -= val;
      opp.life -= val;
    }
  }
  if (strstr(cd.effect, "Enter") && strstr(cd.effect, "Discard")) {
    const char *p = strstr(cd.effect, "Discard");
    if (p) {
      int val = atoi(p + 8);
      for (int i = 0; i < val && mp.handSize > 0; i++)
        MatchDiscard(mp, rand() % mp.handSize);
    }
  }
  if (strstr(cd.effect, "Enter") && strstr(cd.effect, "lose") &&
      strstr(cd.effect, "life") && !strstr(cd.effect, "Opponent")) {
    // "you lose X life" from enter
    const char *p = strstr(cd.effect, "lose");
    if (p && !strstr(cd.effect, "Each player")) {
      int val = atoi(p + 5);
      mp.life -= val;
    }
  }
  if (strstr(cd.effect, "Enter") && strstr(cd.effect, "Sacrifice a unit")) {
    // Sacrifice weakest own unit (not the one just deployed)
    if (mp.fieldSize > 1) {
      int weakest = 0;
      if (weakest == mp.fieldSize - 1)
        weakest = 1;
      for (int i = 0; i < mp.fieldSize - 1; i++)
        if (mp.field[i].curDef < mp.field[weakest].curDef)
          weakest = i;
      mp.field[weakest].alive = false;
    }
    mp.life -= 6;
  }
  if (strstr(cd.effect, "Enter") && strstr(cd.effect, "gain") &&
      strstr(cd.effect, "coin")) {
    const char *p = strstr(cd.effect, "gain");
    if (p) {
      int val = atoi(p + 5);
      mp.coins += val;
    }
  }
  // ── Rare unit Enter effects ──────────────────────────────────────────────
  // ID 141 Shadow Blade: target enemy stops defending
  if (cardId == 141 && opp.fieldSize > 0) {
    // Pick the strongest defender, or any unit
    for (int i = 0; i < opp.fieldSize; i++) {
      if (opp.field[i].isDefender) {
        opp.field[i].isDefender = false;
        break;
      }
    }
  }
  // ID 142 Mind Thief: look at opponent's hand, discard a card, opponent loses
  // life = cost
  if (cardId == 142 && opp.handSize > 0) {
    // AI: discard most expensive card
    int picked = 0;
    for (int i = 1; i < opp.handSize; i++)
      if (GetCard(opp.hand[i]).cost > GetCard(opp.hand[picked]).cost)
        picked = i;
    opp.life -= GetCard(opp.hand[picked]).cost;
    MatchDiscard(opp, picked);
  }
  // ID 143 Rat Lord: add a rat from graveyard to hand
  if (cardId == 143) {
    for (int i = mp.graveSize - 1; i >= 0; i--) {
      if (strstr(GetCard(mp.grave[i]).subtype, "rat") &&
          mp.handSize < MAX_HAND) {
        mp.hand[mp.handSize++] = mp.grave[i];
        for (int j = i; j < mp.graveSize - 1; j++)
          mp.grave[j] = mp.grave[j + 1];
        mp.graveSize--;
        break;
      }
    }
  }
  // ID 147 Verdant Guardian: Enter - Put 4 power counters on each other ally +
  // gain 4 life
  if (cardId == 147) {
    for (int i = 0; i < mp.fieldSize - 1; i++)
      mp.field[i].powerCounters += 4;
    mp.life += 4;
  }
  // ID 150 War Golem: Enter - Put 4 power counters on each other ally + gain 4
  // life
  if (cardId == 150) {
    for (int i = 0; i < mp.fieldSize - 1; i++)
      mp.field[i].powerCounters += 4;
    mp.life += 4;
  }
  // ID 151 Hivemind Matriarch: Enter - Make two 2/2 bug tokens + 2 power on
  // each bug
  if (cardId == 151) {
    for (int t = 0; t < 2 && mp.fieldSize < MAX_FIELD; t++) {
      FieldUnit &tok = mp.field[mp.fieldSize++];
      tok = {};
      tok.cardId = 0;
      tok.curAtk = 2;
      tok.curDef = 2;
      tok.alive = true;
      tok.canActivate = false;
    }
    for (int i = 0; i < mp.fieldSize; i++) {
      if (strstr(GetCard(mp.field[i].cardId).subtype, "bug") ||
          mp.field[i].cardId == 0)
        mp.field[i].powerCounters += 2;
    }
  }
  // ID 154 Arcane Striker: for each card in hand, +1 power counter, then deal
  // that damage
  if (cardId == 154) {
    int handCount = mp.handSize;
    // Put power counters on itself (last field slot)
    mp.field[mp.fieldSize - 1].powerCounters += handCount;
    // Deal handCount damage to weakest enemy
    if (opp.fieldSize > 0) {
      int weakest = 0;
      for (int i = 1; i < opp.fieldSize; i++)
        if (opp.field[i].curDef < opp.field[weakest].curDef)
          weakest = i;
      opp.field[weakest].curDef -= handCount;
      if (opp.field[weakest].curDef <= 0)
        opp.field[weakest].alive = false;
    } else {
      opp.life -= handCount;
    }
  }
  // ID 157 Abyssal Tyrant: Enter - Sacrifice ALL other ally units
  if (cardId == 157) {
    for (int i = 0; i < mp.fieldSize - 1; i++)
      mp.field[i].alive = false;
  }
  // ID 161 Titanic Golem: Enter - Draw 1 card and gain 5 life
  if (cardId == 161) {
    MatchDrawCard(mp);
    mp.life += 5;
  }
  // ID 163 Chrono Mage: Enter - Add up to 1 unit + 1 support from graveyard to
  // hand
  if (cardId == 163) {
    bool addedUnit = false, addedSupport = false;
    for (int i = mp.graveSize - 1; i >= 0 && (!addedUnit || !addedSupport);
         i--) {
      const CardDef &gc = GetCard(mp.grave[i]);
      if (!addedUnit && gc.isUnit && mp.handSize < MAX_HAND) {
        mp.hand[mp.handSize++] = mp.grave[i];
        for (int j = i; j < mp.graveSize - 1; j++)
          mp.grave[j] = mp.grave[j + 1];
        mp.graveSize--;
        addedUnit = true;
        i = mp.graveSize; // restart scan
      } else if (!addedSupport && !gc.isUnit && mp.handSize < MAX_HAND) {
        mp.hand[mp.handSize++] = mp.grave[i];
        for (int j = i; j < mp.graveSize - 1; j++)
          mp.grave[j] = mp.grave[j + 1];
        mp.graveSize--;
        addedSupport = true;
      }
    }
  }
  // ID 167 Berserker Champion: Enter - Discard hand, dig 5, lose 5 life
  if (cardId == 167) {
    while (mp.handSize > 0)
      MatchDiscard(mp, 0);
    // Dig 5: move top 5 from deck to bottom (shuffle into deck)
    // Simplified: draw 2 cards instead of full dig UI
    MatchDrawCard(mp);
    MatchDrawCard(mp);
    mp.life -= 5;
  }
  // ID 172 Apex Predator: Enter - Make a 6/6 beast token
  if (cardId == 172 && mp.fieldSize < MAX_FIELD) {
    FieldUnit &tok = mp.field[mp.fieldSize++];
    tok = {};
    tok.curAtk = 6;
    tok.curDef = 6;
    tok.alive = true;
    tok.canActivate = false;
  }
  // ID 173 Pit Lord: Enter - Destroy an enemy
  if (cardId == 173 && opp.fieldSize > 0) {
    opp.field[rand() % opp.fieldSize].alive = false;
  }
  // ID 174 Archangel of Wrath: Enter - Deal 7 damage to each enemy unit/player
  if (cardId == 174) {
    for (int i = 0; i < opp.fieldSize; i++) {
      opp.field[i].curDef -= 7;
      if (opp.field[i].curDef + opp.field[i].powerCounters -
              opp.field[i].weakCounters <=
          0)
        opp.field[i].alive = false;
    }
    if (opp.fieldSize == 0)
      opp.life -= 7;
  }

  return true;
}

// ── Remove dead units from field ────────────────────────────────────────────
static void CleanupField(MatchPlayer &mp) {
  int w = 0;
  for (int i = 0; i < mp.fieldSize; i++) {
    int effDef = mp.field[i].curDef + mp.field[i].powerCounters -
                 mp.field[i].weakCounters;
    if (!mp.field[i].alive || effDef <= 0) {
      if (mp.graveSize < MAX_GRAVE)
        mp.grave[mp.graveSize++] = mp.field[i].cardId;
      continue;
    }
    if (w != i)
      mp.field[w] = mp.field[i];
    w++;
  }
  mp.fieldSize = w;
}

// ── Helper: deploy a unit from graveyard onto field ────────────────────────
static bool DeployFromGrave(MatchPlayer &mp, int graveIdx,
                            int bonusCounters = 0) {
  if (graveIdx < 0 || graveIdx >= mp.graveSize)
    return false;
  if (mp.fieldSize >= MAX_FIELD)
    return false;
  const CardDef &gc = GetCard(mp.grave[graveIdx]);
  if (!gc.isUnit)
    return false;
  FieldUnit &fu = mp.field[mp.fieldSize++];
  fu = {};
  fu.cardId = mp.grave[graveIdx];
  fu.curAtk = gc.atk;
  fu.curDef = gc.def;
  fu.canActivate = CardHasKeyword(gc, "dash");
  fu.alive = true;
  fu.powerCounters = bonusCounters;
  for (int j = graveIdx; j < mp.graveSize - 1; j++)
    mp.grave[j] = mp.grave[j + 1];
  mp.graveSize--;
  return true;
}

// ── Play Support Card ───────────────────────────────────────────────────────
static void PlaySupportCard(GameMatch &m, int playerIdx, int handIdx) {
  MatchPlayer &mp = m.players[playerIdx];
  MatchPlayer &opp = m.players[1 - playerIdx];
  int cardId = mp.hand[handIdx];
  const CardDef &cd = GetCard(cardId);
  if (mp.coins < cd.cost)
    return;
  mp.coins -= cd.cost;

  // ── Common supports (correct card IDs 108-140) ──────────────────────────
  if (cardId == 108) { // Arrow: Deal 2 damage
    if (opp.fieldSize > 0) {
      int t = rand() % opp.fieldSize;
      opp.field[t].curDef -= 2;
    } else
      opp.life -= 2;
  } else if (cardId == 109) { // Scrying Shovel: Dig 5 (draw 2 simplified)
    MatchDrawCard(mp);
    MatchDrawCard(mp);
  } else if (cardId == 110) { // Ember Wave: 1 damage to each enemy
    for (int i = 0; i < opp.fieldSize; i++) {
      opp.field[i].curDef -= 1;
      if (opp.field[i].curDef + opp.field[i].powerCounters -
              opp.field[i].weakCounters <=
          0)
        opp.field[i].alive = false;
    }
    if (opp.fieldSize == 0)
      opp.life -= 1;
  } else if (cardId == 111) { // Grow: +3 power counters on ally
    if (mp.fieldSize > 0)
      mp.field[rand() % mp.fieldSize].powerCounters += 3;
  } else if (cardId == 112) { // Obsession: Draw 1, lose 4 life
    MatchDrawCard(mp);
    mp.life -= 4;
  } else if (cardId == 113) { // Luck: Gain 1 coin
    mp.coins += 1;
  } else if (cardId == 114) { // Arcane Insight: Draw 2, discard 1
    MatchDrawCard(mp);
    MatchDrawCard(mp);
    if (mp.handSize > 0)
      MatchDiscard(mp, rand() % mp.handSize);
  } else if (cardId == 115) { // Grave Beckoning: add unit from grave to hand
    for (int i = mp.graveSize - 1; i >= 0; i--) {
      if (GetCard(mp.grave[i]).isUnit && mp.handSize < MAX_HAND) {
        mp.hand[mp.handSize++] = mp.grave[i];
        for (int j = i; j < mp.graveSize - 1; j++)
          mp.grave[j] = mp.grave[j + 1];
        mp.graveSize--;
        break;
      }
    }
  } else if (cardId == 116) { // Fire Bolt: Deal 4 damage
    if (opp.fieldSize > 0) {
      int t = rand() % opp.fieldSize;
      opp.field[t].curDef -= 4;
      if (opp.field[t].curDef + opp.field[t].powerCounters -
              opp.field[t].weakCounters <=
          0)
        opp.field[t].alive = false;
    } else
      opp.life -= 4;
  } else if (cardId == 117) { // Spark of Knowledge: 1 damage + draw 1
    if (opp.fieldSize > 0)
      opp.field[rand() % opp.fieldSize].curDef -= 1;
    else
      opp.life -= 1;
    MatchDrawCard(mp);
  } else if (cardId == 118) { // Battle Blessing: +1 power counter + draw 1
    if (mp.fieldSize > 0)
      mp.field[rand() % mp.fieldSize].powerCounters += 1;
    MatchDrawCard(mp);
  } else if (cardId == 119) { // Scorching Tempest: 3 to each unit
    for (int i = 0; i < mp.fieldSize; i++) {
      mp.field[i].curDef -= 3;
      if (mp.field[i].curDef + mp.field[i].powerCounters -
              mp.field[i].weakCounters <=
          0)
        mp.field[i].alive = false;
    }
    for (int i = 0; i < opp.fieldSize; i++) {
      opp.field[i].curDef -= 3;
      if (opp.field[i].curDef + opp.field[i].powerCounters -
              opp.field[i].weakCounters <=
          0)
        opp.field[i].alive = false;
    }
  } else if (cardId == 120) { // Deep Meditation: Draw 2
    MatchDrawCard(mp);
    MatchDrawCard(mp);
  } else if (cardId == 121) { // Golden Harvest: Gain 4 coins
    mp.coins += 4;
  } else if (cardId == 122) { // War Drums: +4 power counters on each ally
    for (int i = 0; i < mp.fieldSize; i++)
      mp.field[i].powerCounters += 4;
  } else if (cardId == 123) { // Annihilate: Destroy an enemy
    if (opp.fieldSize > 0)
      opp.field[rand() % opp.fieldSize].alive = false;
  } else if (cardId == 124) { // Searing Judgment: 4 damage + draw 1
    if (opp.fieldSize > 0) {
      int t = rand() % opp.fieldSize;
      opp.field[t].curDef -= 4;
      if (opp.field[t].curDef + opp.field[t].powerCounters -
              opp.field[t].weakCounters <=
          0)
        opp.field[t].alive = false;
    } else
      opp.life -= 4;
    MatchDrawCard(mp);
  } else if (cardId ==
             125) { // Necromantic Rite: deploy unit cost<=4 from grave
    for (int i = mp.graveSize - 1; i >= 0; i--) {
      const CardDef &gc = GetCard(mp.grave[i]);
      if (gc.isUnit && gc.cost <= 4) {
        DeployFromGrave(mp, i);
        break;
      }
    }
  } else if (cardId == 126) { // Cruel Edict: opponent sacrifices 2 units
    for (int k = 0; k < 2 && opp.fieldSize > 0; k++) {
      opp.field[rand() % opp.fieldSize].alive = false;
      CleanupField(opp);
    }
  } else if (cardId == 127) { // Mind Shatter: opponent discards 2
    for (int k = 0; k < 2 && opp.handSize > 0; k++)
      MatchDiscard(opp, rand() % opp.handSize);
  } else if (cardId == 128) { // Wings of Valor: +3 power + fly until end
    for (int i = 0; i < mp.fieldSize; i++)
      mp.field[i].powerCounters += 3;
  } else if (cardId ==
             129) { // Plague of Frailty: +5 weak counters on each enemy
    for (int i = 0; i < opp.fieldSize; i++)
      opp.field[i].weakCounters += 5;
  } else if (cardId == 130) { // Forbidden Scroll: Draw 4, discard 2
    for (int k = 0; k < 4; k++)
      MatchDrawCard(mp);
    for (int k = 0; k < 2 && mp.handSize > 0; k++)
      MatchDiscard(mp, rand() % mp.handSize);
  } else if (cardId == 131) { // Terminate: Destroy enemy + draw 1
    if (opp.fieldSize > 0)
      opp.field[rand() % opp.fieldSize].alive = false;
    MatchDrawCard(mp);
  } else if (cardId == 132) { // Twin Inferno: 8 damage to up to 2 enemies
    for (int k = 0; k < 2 && opp.fieldSize > 0; k++) {
      int t = rand() % opp.fieldSize;
      opp.field[t].curDef -= 8;
      if (opp.field[t].curDef + opp.field[t].powerCounters -
              opp.field[t].weakCounters <=
          0)
        opp.field[t].alive = false;
      CleanupField(opp);
    }
  } else if (cardId == 133) { // Warcry of the Ancients: +7 power + overrun
    for (int i = 0; i < mp.fieldSize; i++)
      mp.field[i].powerCounters += 7;
  } else if (cardId == 134) { // Wheel of Fate: Discard hand, draw 7
    while (mp.handSize > 0)
      MatchDiscard(mp, 0);
    for (int k = 0; k < 7; k++)
      MatchDrawCard(mp);
  } else if (cardId ==
             135) { // Mass Resurrection: deploy up to 2 cost<=4 from grave
    int count = 0;
    for (int i = mp.graveSize - 1; i >= 0 && count < 2; i--) {
      const CardDef &gc = GetCard(mp.grave[i]);
      if (gc.isUnit && gc.cost <= 4) {
        if (DeployFromGrave(mp, i))
          count++;
        i = mp.graveSize;
      }
    }
  } else if (cardId == 136) { // Rich People's Luck: Gain 7 coins
    mp.coins += 7;
  } else if (cardId == 137) { // Cataclysmic Purge: Destroy up to 3 enemies
    for (int k = 0; k < 3 && opp.fieldSize > 0; k++) {
      opp.field[rand() % opp.fieldSize].alive = false;
      CleanupField(opp);
    }
  } else if (cardId == 138) { // Sovereign's Decree: Draw 2, gain 6 life; opp
                              // discard 2, lose 6 life
    MatchDrawCard(mp);
    MatchDrawCard(mp);
    mp.life += 6;
    for (int k = 0; k < 2 && opp.handSize > 0; k++)
      MatchDiscard(opp, rand() % opp.handSize);
    opp.life -= 6;
  } else if (cardId == 139) { // Hellfire Apocalypse: 6 damage to each enemy,
                              // draw per kill
    int kills = 0;
    for (int i = 0; i < opp.fieldSize; i++) {
      opp.field[i].curDef -= 6;
      if (opp.field[i].curDef + opp.field[i].powerCounters -
              opp.field[i].weakCounters <=
          0) {
        opp.field[i].alive = false;
        kills++;
      }
    }
    if (opp.fieldSize == 0)
      opp.life -= 6;
    for (int k = 0; k < kills; k++)
      MatchDrawCard(mp);
  } else if (cardId == 140) { // Dimensional Rift: deploy from hand + from grave
    // Deploy cheapest unit from hand
    for (int i = 0; i < mp.handSize; i++) {
      if (GetCard(mp.hand[i]).isUnit && mp.fieldSize < MAX_FIELD) {
        // Deploy directly
        const CardDef &gc = GetCard(mp.hand[i]);
        FieldUnit &fu = mp.field[mp.fieldSize++];
        fu = {};
        fu.cardId = mp.hand[i];
        fu.curAtk = gc.atk;
        fu.curDef = gc.def;
        fu.alive = true;
        fu.canActivate = CardHasKeyword(gc, "dash");
        for (int j = i; j < mp.handSize - 1; j++)
          mp.hand[j] = mp.hand[j + 1];
        mp.handSize--;
        break;
      }
    }
    // Deploy from grave
    for (int i = mp.graveSize - 1; i >= 0; i--) {
      if (GetCard(mp.grave[i]).isUnit) {
        DeployFromGrave(mp, i);
        break;
      }
    }
  }
  // ── Rare supports (card IDs 175-193) ───────────────────────────────────
  else if (cardId == 175) { // Mind Shackle: gain control of enemy with atk <= 4
    for (int i = opp.fieldSize - 1; i >= 0; i--) {
      int effAtk = opp.field[i].curAtk + opp.field[i].powerCounters -
                   opp.field[i].weakCounters;
      if (effAtk <= 4 && mp.fieldSize < MAX_FIELD) {
        mp.field[mp.fieldSize++] = opp.field[i];
        mp.field[mp.fieldSize - 1].canActivate = false;
        for (int j = i; j < opp.fieldSize - 1; j++)
          opp.field[j] = opp.field[j + 1];
        opp.fieldSize--;
        break;
      }
    }
  } else if (cardId == 176) { // Power Surge: +5 power on ally, destroy enemy
                              // with less atk
    if (mp.fieldSize > 0) {
      int t = rand() % mp.fieldSize;
      mp.field[t].powerCounters += 5;
      int allyAtk = mp.field[t].curAtk + mp.field[t].powerCounters -
                    mp.field[t].weakCounters;
      for (int i = opp.fieldSize - 1; i >= 0; i--) {
        int enemyAtk = opp.field[i].curAtk + opp.field[i].powerCounters -
                       opp.field[i].weakCounters;
        if (enemyAtk < allyAtk) {
          opp.field[i].alive = false;
          break;
        }
      }
    }
  } else if (cardId ==
             177) { // Arcane Burst: draw 2, discard 1, deal dmg if unit
    MatchDrawCard(mp);
    MatchDrawCard(mp);
    if (mp.handSize > 0) {
      int idx = rand() % mp.handSize;
      const CardDef &dc = GetCard(mp.hand[idx]);
      int dmg = dc.isUnit ? dc.atk : 0;
      MatchDiscard(mp, idx);
      if (dmg > 0) {
        if (opp.fieldSize > 0) {
          int t = rand() % opp.fieldSize;
          opp.field[t].curDef -= dmg;
          if (opp.field[t].curDef + opp.field[t].powerCounters -
                  opp.field[t].weakCounters <=
              0)
            opp.field[t].alive = false;
        } else
          opp.life -= dmg;
      }
    }
  } else if (cardId == 178) { // Celestial Army: for each ally, make 7/7 angel
                              // token with fly
    int count = mp.fieldSize; // snapshot before adding tokens
    for (int k = 0; k < count && mp.fieldSize < MAX_FIELD; k++) {
      FieldUnit &tok = mp.field[mp.fieldSize++];
      tok = {};
      tok.curAtk = 7;
      tok.curDef = 7;
      tok.alive = true;
      tok.canActivate = false;
    }
  } else if (cardId ==
             179) { // War Cry: allies deal double combat damage this turn
    m.warCryActive = true;
  } else if (cardId ==
             180) { // Graveyard Feast: add up to 3 cards from grave to hand
    for (int k = 0; k < 3 && mp.graveSize > 0 && mp.handSize < MAX_HAND; k++) {
      mp.hand[mp.handSize++] = mp.grave[mp.graveSize - 1];
      mp.graveSize--;
    }
  } else if (cardId == 181) { // Domination: gain control of any enemy
    if (opp.fieldSize > 0 && mp.fieldSize < MAX_FIELD) {
      int t = rand() % opp.fieldSize;
      mp.field[mp.fieldSize++] = opp.field[t];
      mp.field[mp.fieldSize - 1].canActivate = false;
      for (int j = t; j < opp.fieldSize - 1; j++)
        opp.field[j] = opp.field[j + 1];
      opp.fieldSize--;
    }
  } else if (cardId == 182) { // Soul Recall: deploy unit from grave
    for (int i = mp.graveSize - 1; i >= 0; i--) {
      if (GetCard(mp.grave[i]).isUnit) {
        DeployFromGrave(mp, i);
        break;
      }
    }
  } else if (cardId == 183) { // Cataclysm: Destroy ALL units
    for (int i = 0; i < mp.fieldSize; i++)
      mp.field[i].alive = false;
    for (int i = 0; i < opp.fieldSize; i++)
      opp.field[i].alive = false;
  } else if (cardId == 184) { // Cerebral Storm: draw 3, deal cards-in-hand dmg
                              // to each enemy
    MatchDrawCard(mp);
    MatchDrawCard(mp);
    MatchDrawCard(mp);
    int dmg = mp.handSize;
    for (int i = 0; i < opp.fieldSize; i++) {
      opp.field[i].curDef -= dmg;
      if (opp.field[i].curDef + opp.field[i].powerCounters -
              opp.field[i].weakCounters <=
          0)
        opp.field[i].alive = false;
    }
    if (opp.fieldSize == 0)
      opp.life -= dmg;
  } else if (cardId ==
             185) { // Mass Revival: deploy all grave units with atk<=4
    for (int i = mp.graveSize - 1; i >= 0; i--) {
      const CardDef &gc = GetCard(mp.grave[i]);
      if (gc.isUnit && gc.atk <= 4) {
        if (DeployFromGrave(mp, i)) {
          i = mp.graveSize;
        }
      }
    }
  } else if (cardId == 186) { // Shadow Summon: deploy a unit from hand (free)
    for (int i = 0; i < mp.handSize; i++) {
      if (GetCard(mp.hand[i]).isUnit && mp.fieldSize < MAX_FIELD) {
        const CardDef &gc = GetCard(mp.hand[i]);
        FieldUnit &fu = mp.field[mp.fieldSize++];
        fu = {};
        fu.cardId = mp.hand[i];
        fu.curAtk = gc.atk;
        fu.curDef = gc.def;
        fu.alive = true;
        fu.canActivate = CardHasKeyword(gc, "dash");
        for (int j = i; j < mp.handSize - 1; j++)
          mp.hand[j] = mp.hand[j + 1];
        mp.handSize--;
        break;
      }
    }
  } else if (cardId == 187) { // Triple Punishment: opp discard 2, sacrifice 2,
                              // lose 4 life
    for (int k = 0; k < 2 && opp.handSize > 0; k++)
      MatchDiscard(opp, rand() % opp.handSize);
    for (int k = 0; k < 2 && opp.fieldSize > 0; k++) {
      opp.field[rand() % opp.fieldSize].alive = false;
      CleanupField(opp);
    }
    opp.life -= 4;
  } else if (cardId == 188) { // Bug Swarm: for each card in hand, make 2/2 bug
                              // + 2 power on each bug
    int handCount = mp.handSize;
    for (int k = 0; k < handCount && mp.fieldSize < MAX_FIELD; k++) {
      FieldUnit &tok = mp.field[mp.fieldSize++];
      tok = {};
      tok.curAtk = 2;
      tok.curDef = 2;
      tok.alive = true;
      tok.canActivate = false;
    }
    for (int i = 0; i < mp.fieldSize; i++)
      mp.field[i].powerCounters += handCount * 2;
  } else if (cardId == 189) { // Annihilation: destroy all enemies
    for (int i = 0; i < opp.fieldSize; i++)
      opp.field[i].alive = false;
  } else if (cardId == 190) { // Grand Bargain: draw 3, gain 6 life, make 12/12
                              // beast token
    MatchDrawCard(mp);
    MatchDrawCard(mp);
    MatchDrawCard(mp);
    mp.life += 6;
    if (mp.fieldSize < MAX_FIELD) {
      FieldUnit &tok = mp.field[mp.fieldSize++];
      tok = {};
      tok.curAtk = 12;
      tok.curDef = 12;
      tok.alive = true;
      tok.canActivate = false;
    }
  } else if (cardId == 191) { // Mass Subjugation: gain control of enemies with
                              // atk<=4 + 4 power counters
    for (int i = opp.fieldSize - 1; i >= 0; i--) {
      int effAtk = opp.field[i].curAtk + opp.field[i].powerCounters -
                   opp.field[i].weakCounters;
      if (effAtk <= 4 && mp.fieldSize < MAX_FIELD) {
        mp.field[mp.fieldSize] = opp.field[i];
        mp.field[mp.fieldSize].powerCounters += 4;
        mp.field[mp.fieldSize].canActivate = false;
        mp.fieldSize++;
        for (int j = i; j < opp.fieldSize - 1; j++)
          opp.field[j] = opp.field[j + 1];
        opp.fieldSize--;
      }
    }
  } else if (cardId == 192) { // Rise from Ashes: deploy each unit from grave
    for (int i = mp.graveSize - 1; i >= 0; i--) {
      if (GetCard(mp.grave[i]).isUnit) {
        DeployFromGrave(mp, i);
        i = mp.graveSize;
      }
    }
  } else if (cardId ==
             193) { // Nature's Army: make six 6/6 beast tokens with overrun
    for (int k = 0; k < 6 && mp.fieldSize < MAX_FIELD; k++) {
      FieldUnit &tok = mp.field[mp.fieldSize++];
      tok = {};
      tok.curAtk = 6;
      tok.curDef = 6;
      tok.alive = true;
      tok.canActivate = false;
    }
  }

  // Remove card from hand to graveyard
  if (mp.graveSize < MAX_GRAVE)
    mp.grave[mp.graveSize++] = cardId;
  for (int i = handIdx; i < mp.handSize - 1; i++)
    mp.hand[i] = mp.hand[i + 1];
  mp.handSize--;

  CleanupField(mp);
  CleanupField(opp);
}

// ── Combat Resolution ───────────────────────────────────────────────────────
static void ResolveCombat(GameMatch &m, int atkPlayer, int atkIdx,
                          int defPlayer, int defIdx) {
  MatchPlayer &ap = m.players[atkPlayer];
  MatchPlayer &dp = m.players[defPlayer];
  FieldUnit &attacker = ap.field[atkIdx];
  const CardDef &acd = GetCard(attacker.cardId);
  int aAtk = attacker.curAtk + attacker.powerCounters - attacker.weakCounters +
             attacker.bonusAtk;
  if (aAtk < 0)
    aAtk = 0;

  if (defIdx < 0) {
    // Direct attack on player
    dp.life -= aAtk;
    attacker.activated = true;
    // Attack trigger effects
    if (strstr(acd.effect, "Attack") && strstr(acd.effect, "Gain") &&
        strstr(acd.effect, "coin")) {
      const char *p = strstr(acd.effect, "Gain");
      if (p)
        ap.coins += atoi(p + 5);
    }
    if (strstr(acd.effect, "Attack") && strstr(acd.effect, "Gain") &&
        strstr(acd.effect, "life")) {
      const char *p = strstr(acd.effect, "Gain");
      if (p)
        ap.life += atoi(p + 5);
    }
    return;
  }

  FieldUnit &defender = dp.field[defIdx];
  const CardDef &dcd = GetCard(defender.cardId);
  int dAtk = defender.curAtk + defender.powerCounters - defender.weakCounters +
             defender.bonusAtk;
  int dDef = defender.curDef + defender.powerCounters - defender.weakCounters +
             defender.bonusDef;
  if (dAtk < 0)
    dAtk = 0;
  if (dDef < 0)
    dDef = 0;
  int aDef = attacker.curDef + attacker.powerCounters - attacker.weakCounters +
             attacker.bonusDef;
  if (aDef < 0)
    aDef = 0;

  attacker.activated = true;

  bool atkTenacity = CardHasKeyword(acd, "tenacity");
  bool defTenacity = CardHasKeyword(dcd, "tenacity");
  bool atkOverrun = CardHasKeyword(acd, "overrun");

  // Compare ATK vs DEF
  if (aAtk > dDef) {
    defender.alive = false;
    if (atkOverrun)
      dp.life -= (aAtk - dDef);
  } else if (aAtk == dDef) {
    defender.alive = false;
    if (!atkTenacity)
      attacker.alive = false;
  } else {
    // Defender survives, reduce DEF temporarily
    defender.curDef -= aAtk;
  }

  // Defender retaliates
  if (dAtk > aDef) {
    attacker.alive = false;
  } else if (dAtk == aDef) {
    if (!defTenacity)
      attacker.alive = false;
    // defender already handled above
  }

  // Attack trigger effects
  if (strstr(acd.effect, "Attack") && strstr(acd.effect, "You lose") &&
      strstr(acd.effect, "life")) {
    const char *p = strstr(acd.effect, "lose");
    if (p)
      ap.life -= atoi(p + 5);
  }
  if (strstr(acd.effect, "Attack") && strstr(acd.effect, "Opponent loses") &&
      strstr(acd.effect, "life")) {
    const char *p = strstr(acd.effect, "loses");
    if (p)
      dp.life -= atoi(p + 6);
  }
  if (strstr(acd.effect, "Attack") && strstr(acd.effect, "Gain") &&
      strstr(acd.effect, "coin")) {
    const char *p = strstr(acd.effect, "Gain");
    if (p)
      ap.coins += atoi(p + 5);
  }
  if (strstr(acd.effect, "Attack") && strstr(acd.effect, "Draw")) {
    const char *p = strstr(acd.effect, "Draw");
    if (p) {
      int val = atoi(p + 5);
      for (int i = 0; i < val; i++)
        MatchDrawCard(ap);
    }
  }
  if (strstr(acd.effect, "Attack") && strstr(acd.effect, "Discard")) {
    if (strstr(acd.effect, "Opponent discards") && dp.handSize > 0)
      MatchDiscard(dp, rand() % dp.handSize);
    else if (ap.handSize > 0)
      MatchDiscard(ap, rand() % ap.handSize);
  }
  if (strstr(acd.effect, "Attack") && strstr(acd.effect, "power counter")) {
    const char *p = strstr(acd.effect, "Put");
    if (p) {
      int val = atoi(p + 4);
      if (strstr(acd.effect, "each other ally")) {
        for (int i = 0; i < ap.fieldSize; i++)
          if (i != atkIdx)
            ap.field[i].powerCounters += val;
      } else if (strstr(acd.effect, "on this unit")) {
        attacker.powerCounters += val;
      }
    }
  }
  // ── Rare unit Attack effects ─────────────────────────────────────────────
  // ID 141 Shadow Blade: Attack - Draw 1 card and lose 1 life
  if (attacker.cardId == 141) {
    MatchDrawCard(ap);
    ap.life -= 1;
  }
  // ID 144 Rat Commander: Attack - Make two 1/1 rat tokens + power on rats per
  // grave rat
  if (attacker.cardId == 144) {
    int ratsInGrave = 0;
    for (int i = 0; i < ap.graveSize; i++) {
      if (strstr(GetCard(ap.grave[i]).subtype, "rat"))
        ratsInGrave++;
    }
    for (int t = 0; t < 2 && ap.fieldSize < MAX_FIELD; t++) {
      FieldUnit &tok = ap.field[ap.fieldSize++];
      tok = {};
      tok.curAtk = 1;
      tok.curDef = 1;
      tok.alive = true;
      tok.canActivate = false;
    }
    for (int i = 0; i < ap.fieldSize; i++) {
      if (i != atkIdx && strstr(GetCard(ap.field[i].cardId).subtype, "rat"))
        ap.field[i].powerCounters += ratsInGrave;
    }
    m.ratsAttackedThisTurn++;
  }
  // ID 149 Golden Golem: Attack - Put a gold counter on self, gain coins per
  // gold counter
  if (attacker.cardId == 149) {
    attacker.goldCounters++;
    ap.coins += attacker.goldCounters;
  }
  // ID 158 Spite Fiend: Attack - Opponent chooses sacrifice OR discard 2 OR
  // lose 8 life (AI: cheapest)
  if (attacker.cardId == 158) {
    // AI logic: prefer discard if has cards, else sacrifice if has units, else
    // lose life
    if (dp.handSize >= 2) {
      MatchDiscard(dp, rand() % dp.handSize);
      MatchDiscard(dp, rand() % dp.handSize);
    } else if (dp.fieldSize > 0) {
      dp.field[rand() % dp.fieldSize].alive = false;
    } else {
      dp.life -= 8;
    }
  }
  // ID 159 Master Infiltrator: Attack - Opponent discards 1, you draw 1
  if (attacker.cardId == 159) {
    if (dp.handSize > 0)
      MatchDiscard(dp, rand() % dp.handSize);
    MatchDrawCard(ap);
  }
  // ID 162 Grave Beetle: Attack - Dig 3 + power counter per grave unit + gain 1
  // life per
  if (attacker.cardId == 162) {
    int graveUnits = 0;
    for (int i = 0; i < ap.graveSize; i++)
      if (GetCard(ap.grave[i]).isUnit)
        graveUnits++;
    attacker.powerCounters += graveUnits;
    ap.life += graveUnits;
    MatchDrawCard(ap); // simplified dig
  }
  // ID 165 Infernal Broodlord: Attack - Make 3/1 demon token with dash
  if (attacker.cardId == 165 && ap.fieldSize < MAX_FIELD) {
    FieldUnit &tok = ap.field[ap.fieldSize++];
    tok = {};
    tok.curAtk = 3;
    tok.curDef = 1;
    tok.alive = true;
    tok.canActivate = true;
  }
  // ID 166 Pack Alpha: Attack - Make 3/3 beast token
  if (attacker.cardId == 166 && ap.fieldSize < MAX_FIELD) {
    FieldUnit &tok = ap.field[ap.fieldSize++];
    tok = {};
    tok.curAtk = 3;
    tok.curDef = 3;
    tok.alive = true;
    tok.canActivate = false;
  }
  // ID 172 Apex Predator: Attack - +6 power counters on each other ally
  if (attacker.cardId == 172) {
    for (int i = 0; i < ap.fieldSize; i++)
      if (i != atkIdx)
        ap.field[i].powerCounters += 6;
  }
  // ID 174 Archangel of Wrath: Attack - Make 7/7 Angel token with fly + gain 7
  // life
  if (attacker.cardId == 174 && ap.fieldSize < MAX_FIELD) {
    FieldUnit &tok = ap.field[ap.fieldSize++];
    tok = {};
    tok.curAtk = 7;
    tok.curDef = 7;
    tok.alive = true;
    tok.canActivate = false;
    ap.life += 7;
  }
  // War Cry double damage (apply to direct attack above too, simplified here)
  (void)m; // suppress unused warning if warCryActive not used here

  CleanupField(ap);
  CleanupField(dp);
}

// ── AI Logic ────────────────────────────────────────────────────────────────
static void AITakeTurn(GameMatch &m) {
  MatchPlayer &ai = m.players[1];
  MatchPlayer &human = m.players[0];

  // Collect phase
  if (!MatchDrawCard(ai)) {
    m.phase = PHASE_GAME_OVER;
    m.playerWon = true;
    m.active = false;
    return;
  }
  ai.coins += 2;

  // Development phase: play most expensive affordable cards
  bool played = true;
  while (played) {
    played = false;
    int bestIdx = -1, bestCost = -1;
    for (int i = 0; i < ai.handSize; i++) {
      const CardDef &cd = GetCard(ai.hand[i]);
      if (cd.isUnit && cd.cost <= ai.coins && ai.fieldSize < MAX_FIELD &&
          cd.cost > bestCost) {
        bestCost = cd.cost;
        bestIdx = i;
      }
    }
    if (bestIdx >= 0) {
      MatchDeployUnit(m, 1, bestIdx);
      CleanupField(ai);
      CleanupField(human);
      played = true;
    }
  }
  // Play affordable support cards
  for (int i = ai.handSize - 1; i >= 0; i--) {
    const CardDef &cd = GetCard(ai.hand[i]);
    if (!cd.isUnit && cd.cost <= ai.coins) {
      PlaySupportCard(m, 1, i);
    }
  }

  // Activation phase: attack with all available units
  // Find defenders first
  bool hasDefenders = false;
  for (int i = 0; i < human.fieldSize; i++)
    if (human.field[i].isDefender) {
      hasDefenders = true;
      break;
    }

  for (int a = 0; a < ai.fieldSize; a++) {
    if (!ai.field[a].canActivate || ai.field[a].activated)
      continue;
    const CardDef &acd = GetCard(ai.field[a].cardId);
    if (strstr(acd.effect, "Cannot defend.") && false)
      continue; // this is about defending, not attacking
    bool hasFly = CardHasKeyword(acd, "fly");

    // Recalculate defenders
    hasDefenders = false;
    for (int i = 0; i < human.fieldSize; i++)
      if (human.field[i].isDefender) {
        hasDefenders = true;
        break;
      }

    if (hasDefenders && !hasFly) {
      // Must attack a defender
      int weakestDef = -1, weakestVal = 99999;
      for (int d = 0; d < human.fieldSize; d++) {
        if (!human.field[d].isDefender)
          continue;
        int val = human.field[d].curDef + human.field[d].powerCounters -
                  human.field[d].weakCounters;
        if (val < weakestVal) {
          weakestVal = val;
          weakestDef = d;
        }
      }
      if (weakestDef >= 0)
        ResolveCombat(m, 1, a, 0, weakestDef);
    } else {
      // Attack weakest unit or player
      if (human.fieldSize > 0 && !hasFly) {
        int weakest = 0;
        for (int d = 1; d < human.fieldSize; d++)
          if (human.field[d].curDef + human.field[d].powerCounters -
                  human.field[d].weakCounters <
              human.field[weakest].curDef + human.field[weakest].powerCounters -
                  human.field[weakest].weakCounters)
            weakest = d;
        ResolveCombat(m, 1, a, 0, weakest);
      } else {
        ResolveCombat(m, 1, a, 0, -1);
      }
    }
    CleanupField(ai);
    CleanupField(human);
    if (human.life <= 0 || ai.life <= 0)
      break;
  }

  // AI assigns defenders: highest DEF units
  for (int i = 0; i < ai.fieldSize; i++)
    ai.field[i].isDefender = false;
  if (ai.fieldSize > 0) {
    // Make the highest DEF units defenders (up to half)
    int numDef = (ai.fieldSize + 1) / 2;
    // Simple: just mark units with highest DEF
    for (int k = 0; k < numDef; k++) {
      int bestIdx = -1, bestDef = -1;
      for (int i = 0; i < ai.fieldSize; i++) {
        if (ai.field[i].isDefender)
          continue;
        const CardDef &cd = GetCard(ai.field[i].cardId);
        if (strstr(cd.effect, "Cannot defend."))
          continue;
        int d = ai.field[i].curDef + ai.field[i].powerCounters -
                ai.field[i].weakCounters;
        if (d > bestDef) {
          bestDef = d;
          bestIdx = i;
        }
      }
      if (bestIdx >= 0)
        ai.field[bestIdx].isDefender = true;
    }
  }

  // End turn: prepare for player
  for (int i = 0; i < ai.fieldSize; i++) {
    ai.field[i].canActivate = true;
    ai.field[i].activated = false;
  }

  // Check win/loss
  if (human.life <= 0) {
    m.phase = PHASE_GAME_OVER;
    m.playerWon = false;
    m.active = false;
    return;
  }
  if (ai.life <= 0) {
    m.phase = PHASE_GAME_OVER;
    m.playerWon = true;
    m.active = false;
    return;
  }
}

// ── Update Match (human turn logic) ─────────────────────────────────────────
static void UpdateMatch(float dt) {
  GameMatch &m = g_match;
  if (!m.active && m.phase == PHASE_GAME_OVER) {
    if (m.messageTimer > 0)
      m.messageTimer -= dt;
    if (IsKeyPressed(KEY_ENTER) || IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
      if (m.playerWon) {
        int reward = (int)(10 * g_inventory.GetWinCoinMult() + 0.5f);
        g_playerCoins += reward;
        // Advance tournament if in tournament mode
        int winDeck[MAX_DECK], loseDeck[MAX_DECK];
        int winCount = m.players[0].deckSize, loseCount = m.players[1].deckSize;
        for (int i = 0; i < winCount; i++)
          winDeck[i] = m.players[0].deck[i];
        for (int i = 0; i < loseCount; i++)
          loseDeck[i] = m.players[1].deck[i];
        g_tournament.AdvanceRound(true, winDeck, winCount, loseDeck, loseCount);
      } else {
        // Lost — tournament doesn't advance but prices still shift
        int winDeck[MAX_DECK], loseDeck[MAX_DECK];
        int winCount = m.players[1].deckSize, loseCount = m.players[0].deckSize;
        for (int i = 0; i < winCount; i++)
          winDeck[i] = m.players[1].deck[i];
        for (int i = 0; i < loseCount; i++)
          loseDeck[i] = m.players[0].deck[i];
        g_tournament.AdvanceRound(false, winDeck, winCount, loseDeck,
                                  loseCount);
      }
      g_scene = SCENE_OVERWORLD;
    }
    return;
  }
  if (m.messageTimer > 0)
    m.messageTimer -= dt;

  MatchPlayer &human = m.players[0];
  MatchPlayer &ai = m.players[1];

  if (m.turn == 0) { // Human turn
    if (m.phase == PHASE_COLLECT) {
      if (!MatchDrawCard(human)) {
        m.phase = PHASE_GAME_OVER;
        m.playerWon = false;
        m.active = false;
        return;
      }
      human.coins += 2;
      m.phase = PHASE_DEVELOP;
      snprintf(m.message, 128, "Your turn - Development Phase (play cards)");
      m.messageTimer = 1.5f;
    }

    if (m.phase == PHASE_DEVELOP) {
      Vector2 mouse = GetMousePosition();

      // Click on hand cards to play them
      for (int i = 0; i < human.handSize; i++) {
        float cx = 60 + i * 95;
        float cy = SCREEN_H - 120;
        Rectangle cardRect = {cx, cy, 85, 110};
        if (CheckCollisionPointRec(mouse, cardRect) &&
            IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
          const CardDef &cd = GetCard(human.hand[i]);
          if (cd.isUnit) {
            if (MatchDeployUnit(m, 0, i)) {
              CleanupField(human);
              CleanupField(ai);
              snprintf(m.message, 128, "Deployed %s", cd.name);
              m.messageTimer = 1.0f;
            } else {
              snprintf(m.message, 128, "Can't deploy: need %d coins (have %d)",
                       cd.cost, human.coins);
              m.messageTimer = 1.0f;
            }
          } else {
            if (human.coins >= cd.cost) {
              PlaySupportCard(m, 0, i);
              snprintf(m.message, 128, "Played %s", cd.name);
              m.messageTimer = 1.0f;
            } else {
              snprintf(m.message, 128, "Need %d coins (have %d)", cd.cost,
                       human.coins);
              m.messageTimer = 1.0f;
            }
          }
          break;
        }
      }

      // Press Space/Enter to advance to Activate phase
      if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) {
        m.phase = PHASE_ACTIVATE;
        m.selectedFieldIdx = -1;
        snprintf(m.message, 128,
                 "Activation Phase - click your units to attack");
        m.messageTimer = 1.5f;
      }
    }

    if (m.phase == PHASE_ACTIVATE) {
      Vector2 mouse = GetMousePosition();

      // Select own unit to attack with
      if (m.selectedFieldIdx < 0) {
        for (int i = 0; i < human.fieldSize; i++) {
          float cx = 60 + i * 110;
          float cy = SCREEN_H / 2 - 30;
          Rectangle unitRect = {cx, cy, 100, 65};
          if (CheckCollisionPointRec(mouse, unitRect) &&
              IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (human.field[i].canActivate && !human.field[i].activated) {
              m.selectedFieldIdx = i;
              snprintf(
                  m.message, 128,
                  "Select target (opponent unit or click 'Attack Player')");
              m.messageTimer = 1.5f;
            } else {
              snprintf(m.message, 128, "Unit can't activate yet");
              m.messageTimer = 1.0f;
            }
            break;
          }
        }
      } else {
        // Select target: opponent unit
        bool hasDefenders = false;
        for (int i = 0; i < ai.fieldSize; i++)
          if (ai.field[i].isDefender) {
            hasDefenders = true;
            break;
          }
        bool atkHasFly = CardHasKeyword(
            GetCard(human.field[m.selectedFieldIdx].cardId), "fly");

        for (int i = 0; i < ai.fieldSize; i++) {
          float cx = 60 + i * 110;
          float cy = 70;
          Rectangle unitRect = {cx, cy, 100, 65};
          if (CheckCollisionPointRec(mouse, unitRect) &&
              IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (hasDefenders && !ai.field[i].isDefender && !atkHasFly) {
              snprintf(m.message, 128, "Must attack defenders first!");
              m.messageTimer = 1.0f;
            } else {
              ResolveCombat(m, 0, m.selectedFieldIdx, 1, i);
              m.selectedFieldIdx = -1;
              snprintf(m.message, 128, "Combat resolved!");
              m.messageTimer = 1.0f;
            }
            break;
          }
        }

        // Attack player button
        Rectangle atkPlayerBtn = {(float)SCREEN_W - 180, 20, 160, 30};
        if (CheckCollisionPointRec(mouse, atkPlayerBtn) &&
            IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
          if (hasDefenders && !atkHasFly) {
            snprintf(m.message, 128, "Must destroy all defenders first!");
            m.messageTimer = 1.0f;
          } else {
            ResolveCombat(m, 0, m.selectedFieldIdx, 1, -1);
            m.selectedFieldIdx = -1;
            snprintf(m.message, 128, "Direct attack!");
            m.messageTimer = 1.0f;
          }
        }

        // Right-click to cancel selection
        if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
          m.selectedFieldIdx = -1;
        }
      }

      // Assign defenders: click 'D' to toggle defender on selected unit
      if (IsKeyPressed(KEY_D)) {
        for (int i = 0; i < human.fieldSize; i++) {
          float cx = 60 + i * 110;
          float cy = SCREEN_H / 2 - 30;
          Rectangle unitRect = {cx, cy, 100, 65};
          if (CheckCollisionPointRec(mouse, unitRect)) {
            const CardDef &cd = GetCard(human.field[i].cardId);
            if (!strstr(cd.effect, "Cannot defend."))
              human.field[i].isDefender = !human.field[i].isDefender;
            break;
          }
        }
      }

      // Press Space/Enter to end turn
      if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) {
        m.phase = PHASE_END;
      }
    }

    if (m.phase == PHASE_END) {
      // Reset activation for next turn
      for (int i = 0; i < human.fieldSize; i++) {
        human.field[i].canActivate = true;
        human.field[i].activated = false;
      }
      // Check win/loss
      if (ai.life <= 0) {
        m.phase = PHASE_GAME_OVER;
        m.playerWon = true;
        m.active = false;
        snprintf(m.message, 128, "You win! Press Enter to continue.");
        m.messageTimer = 99.0f;
        return;
      }
      if (human.life <= 0) {
        m.phase = PHASE_GAME_OVER;
        m.playerWon = false;
        m.active = false;
        snprintf(m.message, 128, "You lose! Press Enter to continue.");
        m.messageTimer = 99.0f;
        return;
      }

      // AI turn
      m.turn = 1;
      m.turnNumber++;
      AITakeTurn(m);

      // Check again
      if (ai.life <= 0) {
        m.phase = PHASE_GAME_OVER;
        m.playerWon = true;
        m.active = false;
        snprintf(m.message, 128, "You win! Press Enter to continue.");
        m.messageTimer = 99.0f;
        return;
      }
      if (human.life <= 0) {
        m.phase = PHASE_GAME_OVER;
        m.playerWon = false;
        m.active = false;
        snprintf(m.message, 128, "You lose! Press Enter to continue.");
        m.messageTimer = 99.0f;
        return;
      }

      // Back to player
      m.turn = 0;
      m.phase = PHASE_COLLECT;
    }
  }
}

// ── Draw Match Scene ────────────────────────────────────────────────────────
static void DrawMatchScene() {
  GameMatch &m = g_match;
  MatchPlayer &human = m.players[0];
  MatchPlayer &ai = m.players[1];

  // The board is a flat surface placed in 3D space.
  DrawPlane((Vector3){0, -0.2f, 0}, (Vector2){28, 22}, {10, 20, 15, 255});
  DrawPlane((Vector3){0, -0.1f, 0}, (Vector2){26, 20}, {90, 70, 40, 255});
  DrawCubeWires((Vector3){0, -0.1f, 0}, 26, 0.01f, 20, {140, 110, 60, 255});

  // Divider line
  DrawLine3D((Vector3){-12, 0, 0}, (Vector3){12, 0, 0}, {140, 110, 60, 200});

  // Opponent cards appear farther away due to perspective.
  float oppFieldZ = 4.0f;
  float oppHandZ = 6.0f;
  float cardWidth = 2.0f;
  float cardHeight = 1.5f;

  // ── Opponent hand (face-down cards)
  for (int i = 0; i < ai.handSize; i++) {
    float totalWidth = ai.handSize * 1.2f;
    float startX = -totalWidth / 2.0f + 0.6f;
    float cx = startX + i * 1.2f;
    DrawCube((Vector3){cx, 0, oppHandZ}, 1.0f, 0.1f, 0.7f, {80, 60, 40, 255});
    DrawCubeWires((Vector3){cx, 0, oppHandZ}, 1.0f, 0.1f, 0.7f,
                  {120, 100, 60, 255});
  }

  // ── Opponent field (units)
  for (int i = 0; i < ai.fieldSize; i++) {
    float totalWidth = ai.fieldSize * (cardWidth + 0.2f);
    float startX = -totalWidth / 2.0f + cardWidth / 2.0f;
    float cx = startX + i * (cardWidth + 0.2f);
    Color bg = ai.field[i].isDefender ? Color{60, 60, 120, 255}
                                      : Color{70, 50, 35, 255};
    DrawCube((Vector3){cx, 0, oppFieldZ}, cardWidth, 0.2f, cardHeight, bg);
    DrawCubeWires((Vector3){cx, 0, oppFieldZ}, cardWidth, 0.2f, cardHeight,
                  {180, 150, 80, 255});
  }

  // Player cards are positioned slightly closer to the camera.
  float playerFieldZ = -4.0f;
  float playerHandZ = -9.0f;

  // ── Player field (units)
  for (int i = 0; i < human.fieldSize; i++) {
    float totalWidth = human.fieldSize * (cardWidth + 0.2f);
    float startX = -totalWidth / 2.0f + cardWidth / 2.0f;
    float cx = startX + i * (cardWidth + 0.2f);
    Color bg = {50, 40, 30, 255};
    if (human.field[i].isDefender)
      bg = {40, 40, 90, 255};
    if (m.selectedFieldIdx == i)
      bg = {100, 80, 40, 255};
    if (!human.field[i].canActivate)
      bg = {40, 35, 25, 255};
    DrawCube((Vector3){cx, 0, playerFieldZ}, cardWidth, 0.2f, cardHeight, bg);
    Color border = (human.field[i].canActivate && !human.field[i].activated)
                       ? Color{255, 220, 80, 255}
                       : Color{120, 100, 60, 255};
    DrawCubeWires((Vector3){cx, 0, playerFieldZ}, cardWidth, 0.2f, cardHeight,
                  border);
  }

  // ── Player hand (full cards)
  float handCardWidth = 2.2f;
  float handCardHeight = 3.0f;
  for (int i = 0; i < human.handSize; i++) {
    const CardDef &cd = GetCard(human.hand[i]);
    float totalWidth = human.handSize * (handCardWidth + 0.2f);
    float startX = -totalWidth / 2.0f + handCardWidth / 2.0f;
    float cx = startX + i * (handCardWidth + 0.2f);
    Color bg = cd.isUnit ? Color{50, 60, 50, 255} : Color{60, 50, 60, 255};
    if (cd.cost > human.coins)
      bg = {40, 30, 30, 255}; // dim if can't afford

    // NOTE: Drawing text and detailed card art in a 3D perspective requires a
    // more complex solution (e.g., rendering cards to a texture and drawing
    // that texture). For this 3D transformation, we are only drawing colored
    // cubes as placeholders.
    DrawCube((Vector3){cx, 0, playerHandZ}, handCardWidth, 0.1f, handCardHeight,
             bg);
    DrawCubeWires((Vector3){cx, 0, playerHandZ}, handCardWidth, 0.1f,
                  handCardHeight, {160, 140, 80, 255});
  }
}

// ── Shop System ─────────────────────────────────────────────────────────────
static void UpdateShop(float dt) {
  (void)dt;
  Vector2 mouse = GetMousePosition();

  // Buy starter deck button
  if (!g_hasStarterDeck) {
    Rectangle btn = {SCREEN_W / 2 - 100, 200, 200, 40};
    if (CheckCollisionPointRec(mouse, btn) &&
        IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
      GiveStarterDeck();
    }
  }

  // Buy pack button — price scales with day/night cycle
  int packPrice = (int)(10 * g_worldClock.GetShopMod() + 0.5f);
  Rectangle packBtn = {SCREEN_W / 2 - 100, 260, 200, 40};
  if (CheckCollisionPointRec(mouse, packBtn) &&
      IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
    if (g_playerCoins >= packPrice) {
      g_playerCoins -= packPrice;
      // Add 10 random cards with rarity weighting
      bool hasFairScale = g_inventory.HasFairScale();
      for (int i = 0; i < 10; i++) {
        int cardId;
        float roll = (float)rand() / RAND_MAX;
        if (hasFairScale)
          roll *= 0.5f; // doubles chance of rare+
        if (roll < 0.10f)
          cardId = ALL_CARDS[140 + rand() % 53].id; // rare cards (id 141-193)
        else if (roll < 0.25f)
          cardId = ALL_CARDS[91 + rand() % 49]
                       .id; // unique commons (cost 5-6 + supports)
        else
          cardId = ALL_CARDS[rand() % 91].id; // regular commons (cost 1-4)
        if (g_collectionSize < MAX_COLLECTION)
          g_collection[g_collectionSize++] = cardId;
      }
      g_market.OnPackOpened();
    }
  }

  // Sell card — click collection entry to sell
  // (handled in draw with sell buttons)

  // Scroll collection
  UpdateController(GetFrameTime());

  if (IsKeyDown(KEY_DOWN) || (g_gamepadConnected && g_dpadY > 0.5f))
    g_shopScroll++;
  if ((IsKeyDown(KEY_UP) || (g_gamepadConnected && g_dpadY < -0.5f)) &&
      g_shopScroll > 0)
    g_shopScroll--;

  // Exit shop
  if (IsKeyPressed(KEY_ESCAPE)) {
    g_scene = SCENE_OVERWORLD;
  }
}

static void DrawShopScene() {
  DrawRectangle(0, 0, SCREEN_W, SCREEN_H, {50, 40, 28, 255});
  DrawText("MERCHANT'S CARD SHOP", SCREEN_W / 2 - 120, 20, 20,
           {255, 220, 120, 255});

  char buf[128];
  snprintf(buf, 128, "Your Coins: %d    Collection: %d cards    Deck: %d cards",
           g_playerCoins, g_collectionSize, g_playerDeckSize);
  DrawText(buf, SCREEN_W / 2 - 200, 60, 12, {200, 180, 140, 255});

  // Starter deck button
  if (!g_hasStarterDeck) {
    DrawRectangleRounded({(float)SCREEN_W / 2 - 100, 200, 200, 40}, 0.3f, 4,
                         {60, 120, 60, 255});
    DrawText("Get Starter Deck (FREE)", SCREEN_W / 2 - 85, 212, 12,
             {255, 255, 200, 255});
  } else {
    DrawText("Starter deck acquired!", SCREEN_W / 2 - 80, 210, 12,
             {120, 200, 120, 200});
  }

  // Pack button with dynamic price
  int packPrice = (int)(10 * g_worldClock.GetShopMod() + 0.5f);
  Color packCol = (g_playerCoins >= packPrice) ? Color{80, 80, 140, 255}
                                               : Color{50, 50, 60, 255};
  DrawRectangleRounded({(float)SCREEN_W / 2 - 100, 260, 200, 40}, 0.3f, 4,
                       packCol);
  snprintf(buf, 128, "Buy Card Pack (%d coins)", packPrice);
  DrawText(buf, SCREEN_W / 2 - 85, 272, 12, {255, 255, 200, 255});

  // Time of day indicator
  snprintf(buf, 128, "Time: %s (price mod: %.0f%%)", g_worldClock.GetName(),
           (g_worldClock.GetShopMod() - 1.0f) * 100);
  DrawText(buf, SCREEN_W / 2 - 100, 95, 10, {180, 160, 120, 200});

  // Collection display with sell prices
  DrawText("Your Collection (click [S] next to card to sell):", 30, 320, 14,
           {220, 200, 160, 255});
  int startIdx = g_shopScroll * 8;
  int y = 340;
  float dayMod = g_worldClock.GetShopMod();
  float sellMult = g_inventory.GetSellMult();
  for (int i = startIdx; i < g_collectionSize && y < SCREEN_H - 30; i++) {
    const CardDef &cd = GetCard(g_collection[i]);
    int sellPrice = g_market.GetSellPrice(g_collection[i], dayMod, sellMult);
    int buyPrice = g_market.GetBuyPrice(g_collection[i], dayMod);
    snprintf(buf, 128, "%s (Cost:%d %s) Buy:%d Sell:%d", cd.name, cd.cost,
             cd.isUnit ? cd.subtype : "Support", buyPrice, sellPrice);
    DrawText(buf, 40, y, 10, {180, 170, 140, 220});

    // Sell button
    Rectangle sellBtn = {(float)SCREEN_W - 80, (float)y - 2, 50, 14};
    DrawRectangleRounded(sellBtn, 0.3f, 4, {140, 60, 60, 255});
    DrawText("SELL", SCREEN_W - 72, y, 8, {255, 220, 180, 255});
    if (CheckCollisionPointRec(GetMousePosition(), sellBtn) &&
        IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
      g_playerCoins += sellPrice;
      g_market.OnCardSold(g_collection[i]);
      // Remove card from collection
      for (int j = i; j < g_collectionSize - 1; j++)
        g_collection[j] = g_collection[j + 1];
      g_collectionSize--;
    }
    y += 16;
  }

  DrawText("[Esc] Exit shop    [UP/DOWN] Scroll    Click SELL to sell cards",
           20, SCREEN_H - 25, 10, {150, 130, 100, 150});
}

// ═══════════════════════════════════════════════════════════════════════════════
// CARD FRAME GENERATION — Pixel-art card textures (96x128)
// ═══════════════════════════════════════════════════════════════════════════════
static constexpr int CARD_TEX_W = 96;
static constexpr int CARD_TEX_H = 128;
static Texture2D g_cardTextures[200]; // one per card ID (up to 193 + extra)
static int g_numCardTextures = 0;

// Desert-themed 16-color pixel palette
static const Color PALETTE[] = {
    {30, 22, 15, 255},    // 0: dark bg
    {60, 45, 25, 255},    // 1: dark brown frame
    {110, 85, 50, 255},   // 2: mid brown
    {165, 130, 70, 255},  // 3: gold
    {210, 175, 90, 255},  // 4: bright gold
    {240, 220, 150, 255}, // 5: cream
    {255, 245, 200, 255}, // 6: white cream
    {180, 50, 40, 255},   // 7: red (atk)
    {60, 80, 160, 255},   // 8: blue (def)
    {60, 150, 80, 255},   // 9: green (support)
    {130, 70, 150, 255},  // 10: purple (demon)
    {200, 160, 80, 255},  // 11: tan
    {80, 60, 40, 255},    // 12: earth brown
    {45, 35, 20, 255},    // 13: near black
    {220, 200, 100, 255}, // 14: gold highlight
    {140, 120, 80, 255},  // 15: muted gold
};

// Get subtype color for card art area
static Color SubtypeColor(const char *sub) {
  if (!sub || !sub[0])
    return PALETTE[9]; // support = green
  if (strstr(sub, "rat") || strstr(sub, "beast"))
    return {160, 120, 70, 255};
  if (strstr(sub, "plant"))
    return {70, 130, 60, 255};
  if (strstr(sub, "golem"))
    return {130, 130, 140, 255};
  if (strstr(sub, "bug"))
    return {100, 140, 60, 255};
  if (strstr(sub, "berserker"))
    return {180, 60, 50, 255};
  if (strstr(sub, "mage"))
    return {80, 70, 160, 255};
  if (strstr(sub, "angel") || strstr(sub, "spirit"))
    return {180, 180, 220, 255};
  if (strstr(sub, "merchant"))
    return {190, 170, 80, 255};
  if (strstr(sub, "demon"))
    return {120, 50, 140, 255};
  if (strstr(sub, "zombie"))
    return {90, 100, 70, 255};
  if (strstr(sub, "rogue") || strstr(sub, "soldier"))
    return {140, 110, 80, 255};
  if (strstr(sub, "wurm"))
    return {140, 100, 60, 255};
  return {120, 100, 70, 255};
}

// Generate a unique pixel-art creature based on card id (deterministic hash
// art)
static void DrawCardCreature(Image *img, int cardId, Color baseCol, int ox,
                             int oy, int w, int h) {
  unsigned seed = (unsigned)(cardId * 2654435761u);
  int bw = w / 3 + (seed % 3);
  int bh = h / 3 + ((seed >> 4) % 4);
  int bx = ox + (w - bw) / 2;
  int by = oy + h - bh - 2;
  Color body = baseCol;
  Color dark = {(unsigned char)(body.r * 0.6f), (unsigned char)(body.g * 0.6f),
                (unsigned char)(body.b * 0.6f), 255};
  Color light = {(unsigned char)Clamp(body.r * 1.3f, 0, 255),
                 (unsigned char)Clamp(body.g * 1.3f, 0, 255),
                 (unsigned char)Clamp(body.b * 1.3f, 0, 255), 255};
  ImageDrawRectangle(img, bx, by, bw, bh, body);
  ImageDrawRectangle(img, bx, by, bw, 1, light);
  ImageDrawRectangle(img, bx, by + bh - 1, bw, 1, dark);
  int hw2 = bw / 2 + ((seed >> 8) % 2);
  int hh2 = bh / 2 + ((seed >> 12) % 2);
  int hx = bx + (bw - hw2) / 2;
  int hy = by - hh2 + 1;
  ImageDrawRectangle(img, hx, hy, hw2, hh2, body);
  ImageDrawRectangle(img, hx, hy, hw2, 1, light);
  int ey = hy + hh2 / 3;
  ImageDrawPixel(img, hx + hw2 / 4, ey, PALETTE[0]);
  ImageDrawPixel(img, hx + hw2 * 3 / 4, ey, PALETTE[0]);
  if ((seed >> 16) & 1) {
    ImageDrawRectangle(img, bx - 3, by + 1, 3, bh / 2, dark);
    ImageDrawRectangle(img, bx + bw, by + 1, 3, bh / 2, dark);
  }
  if ((seed >> 17) & 1) {
    ImageDrawRectangle(img, bx + bw, by + bh - 3, 4, 2, dark);
  }
  if ((seed >> 18) & 1) {
    ImageDrawPixel(img, hx + 1, hy - 1, dark);
    ImageDrawPixel(img, hx + hw2 - 2, hy - 1, dark);
  }
  ImageDrawRectangle(img, bx + 1, by + bh, 2, 3, dark);
  ImageDrawRectangle(img, bx + bw - 3, by + bh, 2, 3, dark);
}

static void GenerateCardTexture(int cardIdx) {
  const CardDef &cd = ALL_CARDS[cardIdx];
  Image img = GenImageColor(CARD_TEX_W, CARD_TEX_H, PALETTE[0]);

  // Outer frame (2px border — gold for common, purple for rare, rainbow for
  // unique)
  Color frameCol = cd.rarity == 1 ? Color{180, 80, 220, 255}
                   : cd.isUnique  ? Color{220, 180, 60, 255}
                                  : PALETTE[3];
  ImageDrawRectangleLines(&img, {0, 0, (float)CARD_TEX_W, (float)CARD_TEX_H}, 2,
                          frameCol);
  ImageDrawRectangleLines(
      &img, {2, 2, (float)(CARD_TEX_W - 4), (float)CARD_TEX_H - 4}, 1,
      PALETTE[1]);

  // Top bar: name background
  ImageDrawRectangle(&img, 3, 3, CARD_TEX_W - 6, 14, PALETTE[1]);
  ImageDrawText(&img, cd.name, 16, 5, 8, PALETTE[5]);

  // Cost circle (top-left)
  Color costBg = cd.isUnit ? PALETTE[3] : PALETTE[9];
  ImageDrawRectangle(&img, 4, 4, 11, 11, costBg);
  ImageDrawRectangleLines(&img, {4, 4, 11, 11}, 1, PALETTE[0]);
  char costStr[4];
  snprintf(costStr, 4, "%d", cd.cost);
  ImageDrawText(&img, costStr, 6, 5, 8, PALETTE[6]);

  // Art area (top half below name bar)
  int artY = 18;
  int artH = 50;
  Color artBg = cd.isUnit ? SubtypeColor(cd.subtype) : Color{60, 90, 50, 255};
  Color artBgDk = {(unsigned char)(artBg.r * 0.4f),
                   (unsigned char)(artBg.g * 0.4f),
                   (unsigned char)(artBg.b * 0.4f), 255};
  ImageDrawRectangle(&img, 4, artY, CARD_TEX_W - 8, artH, artBgDk);
  ImageDrawRectangleLines(
      &img, {4, (float)artY, (float)(CARD_TEX_W - 8), (float)artH}, 1,
      PALETTE[2]);

  // Draw creature/spell art
  if (cd.isUnit) {
    DrawCardCreature(&img, cd.id, artBg, 8, artY + 4, CARD_TEX_W - 16,
                     artH - 8);
  } else {
    int cx2 = CARD_TEX_W / 2, cy2 = artY + artH / 2;
    for (int a = 0; a < 8; a++) {
      float angle = a * 0.785f;
      for (int r = 3; r < 12; r++) {
        int px = cx2 + (int)(cosf(angle) * r);
        int py = cy2 + (int)(sinf(angle) * r);
        if (px >= 4 && px < CARD_TEX_W - 4 && py >= artY && py < artY + artH) {
          Color sc = (r < 7) ? PALETTE[4] : PALETTE[14];
          ImageDrawPixel(&img, px, py, sc);
        }
      }
    }
  }

  // Divider line
  int divY = artY + artH + 1;
  ImageDrawRectangle(&img, 4, divY, CARD_TEX_W - 8, 1, PALETTE[3]);

  // Subtype text
  if (cd.isUnit && cd.subtype[0]) {
    char subBuf[32];
    snprintf(subBuf, 32, "[%s]", cd.subtype);
    ImageDrawText(&img, subBuf, 6, divY + 2, 8, PALETTE[15]);
  } else {
    ImageDrawText(&img, "[Support]", 6, divY + 2, 8, PALETTE[9]);
  }

  // Rules text area
  int textY = divY + 12;
  int textH = CARD_TEX_H - textY - 18;
  ImageDrawRectangle(&img, 4, textY, CARD_TEX_W - 8, textH, {40, 30, 18, 255});
  if (cd.effect[0]) {
    char line1[48] = {}, line2[48] = {};
    int len = (int)strlen(cd.effect);
    if (len <= 20) {
      snprintf(line1, 47, "%s", cd.effect);
    } else {
      int brk = 20;
      while (brk > 10 && cd.effect[brk] != ' ')
        brk--;
      if (brk <= 10)
        brk = 20;
      snprintf(line1, brk + 1, "%s", cd.effect);
      snprintf(line2, 47, "%s",
               cd.effect + brk + (cd.effect[brk] == ' ' ? 1 : 0));
    }
    ImageDrawText(&img, line1, 6, textY + 2, 8, PALETTE[5]);
    if (line2[0])
      ImageDrawText(&img, line2, 6, textY + 11, 8, PALETTE[5]);
  }

  // Stat blocks (units only)
  if (cd.isUnit) {
    int statY = CARD_TEX_H - 16;
    ImageDrawRectangle(&img, 3, statY, 22, 14, {140, 40, 30, 255});
    ImageDrawRectangleLines(&img, {3, (float)statY, 22, 14}, 1, PALETTE[0]);
    char atkStr[8];
    snprintf(atkStr, 8, "%d", cd.atk);
    ImageDrawText(&img, atkStr, 7, statY + 3, 8, PALETTE[6]);
    ImageDrawPixel(&img, 5, statY + 2, PALETTE[4]);
    ImageDrawPixel(&img, 5, statY + 3, PALETTE[4]);

    ImageDrawRectangle(&img, CARD_TEX_W - 25, statY, 22, 14,
                       {40, 50, 140, 255});
    ImageDrawRectangleLines(
        &img, {(float)(CARD_TEX_W - 25), (float)statY, 22, 14}, 1, PALETTE[0]);
    char defStr[8];
    snprintf(defStr, 8, "%d", cd.def);
    ImageDrawText(&img, defStr, CARD_TEX_W - 21, statY + 3, 8, PALETTE[6]);
    ImageDrawPixel(&img, CARD_TEX_W - 23, statY + 2, PALETTE[8]);
    ImageDrawPixel(&img, CARD_TEX_W - 23, statY + 3, PALETTE[8]);
  } else {
    int statY = CARD_TEX_H - 14;
    char cstr[16];
    snprintf(cstr, 16, "Cost: %d", cd.cost);
    ImageDrawText(&img, cstr, CARD_TEX_W / 2 - 20, statY + 2, 8, PALETTE[15]);
  }

  // Keywords strip (above stats)
  if (cd.keywords[0]) {
    int kwY = CARD_TEX_H - 28;
    ImageDrawText(&img, cd.keywords, 6, kwY, 8, PALETTE[14]);
  }

  Texture2D tex = LoadTextureFromImage(img);
  SetTextureFilter(tex, TEXTURE_FILTER_POINT);
  UnloadImage(img);
  g_cardTextures[cd.id] = tex;
}

static void GenerateAllCardTextures() {
  memset(g_cardTextures, 0, sizeof(g_cardTextures));
  for (int i = 0; i < NUM_ALL_CARDS; i++) {
    GenerateCardTexture(i);
  }
  g_numCardTextures = NUM_ALL_CARDS;
}

// ═══════════════════════════════════════════════════════════════════════════════
// NPC DIALOG — RPG-style bottom-screen Name Box + Message Box
// ═══════════════════════════════════════════════════════════════════════════════
static void DrawNPCDialog() {
  if (!g_npcDialogOpen || g_targetNPC < 0)
    return;

  // ── Layout constants ───────────────────────────────────────────────────
  const int boxMargin = 24;
  const int msgBoxH = 100;
  const int msgBoxY = SCREEN_H - msgBoxH - boxMargin;
  const int msgBoxX = boxMargin;
  const int msgBoxW = SCREEN_W - boxMargin * 2;

  const int nameBoxH = 28;
  const int nameBoxY = msgBoxY - nameBoxH + 2; // overlaps top edge slightly
  const int nameBoxX = msgBoxX + 12;

  // ── Name Box (semi-transparent, above message box) ─────────────────────
  const char *name = g_npcs[g_targetNPC].name;
  int nameW = MeasureText(name, 16) + 24;
  DrawRectangle(nameBoxX, nameBoxY, nameW, nameBoxH, {15, 10, 8, 220});
  DrawRectangleLinesEx({(float)nameBoxX, (float)nameBoxY,
                        (float)nameW, (float)nameBoxH}, 1.5f,
                       {200, 170, 100, 255});
  DrawText(name, nameBoxX + 12, nameBoxY + 6, 16, {255, 230, 150, 255});

  // ── Message Box (main dialogue area) ───────────────────────────────────
  DrawRectangle(msgBoxX, msgBoxY, msgBoxW, msgBoxH, {18, 13, 9, 230});
  DrawRectangleLinesEx({(float)msgBoxX, (float)msgBoxY,
                        (float)msgBoxW, (float)msgBoxH}, 2.0f,
                       {200, 170, 100, 255});

  if (g_dialogPhase == DIALOG_TALK && g_dialogText) {
    // ── Talk phase: show lore text ─────────────────────────────────────
    DrawText(g_dialogText, msgBoxX + 18, msgBoxY + 14, 14,
             {230, 220, 190, 255});
    DrawText("[Enter] OK", msgBoxX + msgBoxW - 100, msgBoxY + msgBoxH - 22,
             10, {180, 160, 120, 180});
  } else {
    // ── Menu phase: Talk / Trade / Duel / Close ────────────────────────
    const char *options[] = {"Talk", "Trade", "Duel", "Close"};
    for (int i = 0; i < 4; i++) {
      bool sel = (i == g_dialogSelection);
      Color col = sel ? Color{255, 220, 80, 255} : Color{210, 200, 170, 255};
      const char *arrow = sel ? "> " : "  ";
      char buf[64];
      snprintf(buf, sizeof(buf), "%s%s", arrow, options[i]);
      DrawText(buf, msgBoxX + 18, msgBoxY + 14 + i * 20, 16, col);
    }
    DrawText("[W/S] Select  [Enter/E] Confirm  [Esc] Close",
             msgBoxX + 18, msgBoxY + msgBoxH - 22, 10,
             {160, 140, 100, 160});
  }
}

static void InteractWithNPC(int npcIdx);

static void UpdateNPCDialog() {
  if (!g_npcDialogOpen)
    return;

  bool confirm = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_E) || g_padAPressed;

  if (g_dialogPhase == DIALOG_TALK) {
    // In talk mode, any confirm/escape dismisses back to menu
    if (confirm || IsKeyPressed(KEY_ESCAPE) || g_padBPressed) {
      g_dialogPhase = DIALOG_MENU;
      g_dialogText = nullptr;
    }
    return;
  }

  // ── Menu navigation ────────────────────────────────────────────────────
  if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) {
    g_dialogSelection--;
    if (g_dialogSelection < 0) g_dialogSelection = 3;
  }
  if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) {
    g_dialogSelection++;
    if (g_dialogSelection > 3) g_dialogSelection = 0;
  }

  if (confirm) {
    switch (g_dialogSelection) {
    case 0: { // Talk — show random card-game lore
      int line = GetRandomValue(0, 2);
      int idx = (g_targetNPC >= 0 && g_targetNPC < 4) ? g_targetNPC : 0;
      g_dialogText = g_npcLore[idx][line];
      g_dialogPhase = DIALOG_TALK;
      break;
    }
    case 1: // Trade
      if (g_targetNPC == 2) { // Merchant Yara
        g_scene = SCENE_SHOP;
        g_npcDialogOpen = false;
        g_dialogPhase = DIALOG_MENU;
      } else {
        g_dialogText = "I have nothing to trade right now.\nTry the merchant at Yara's Bazaar.";
        g_dialogPhase = DIALOG_TALK;
      }
      break;
    case 2: // Duel
      if (!g_hasStarterDeck) {
        g_dialogText = "You need a deck first! Visit the bazaar\nand pick up a starter pack.";
        g_dialogPhase = DIALOG_TALK;
      } else if (g_playerDeckSize >= 20) {
        StartMatch(g_targetNPC);
        g_scene = SCENE_MATCH;
        g_npcDialogOpen = false;
        g_dialogPhase = DIALOG_MENU;
      } else {
        g_dialogText = "Your deck needs at least 20 cards.\nOpen some packs at the bazaar!";
        g_dialogPhase = DIALOG_TALK;
      }
      break;
    case 3: // Close
      g_npcDialogOpen = false;
      g_dialogPhase = DIALOG_MENU;
      g_dialogText = nullptr;
      break;
    }
  }

  if (IsKeyPressed(KEY_ESCAPE) || g_padBPressed) {
    g_npcDialogOpen = false;
    g_dialogPhase = DIALOG_MENU;
    g_dialogText = nullptr;
  }
}

// ── InteractWithNPC — called when player presses Enter/E near an NPC ─────────
//    Opens dialogue, makes NPC face the player, resets dialogue state.
static void InteractWithNPC(int npcIdx) {
  g_npcDialogOpen = true;
  g_targetNPC = npcIdx;
  g_dialogSelection = 0;
  g_dialogPhase = DIALOG_MENU;
  g_dialogText = nullptr;
  // NPC instantly faces the player
  float dx = g_player.posX - g_npcs[npcIdx].worldX;
  float dz = g_player.posZ - g_npcs[npcIdx].worldZ;
  float angle = atan2f(dx, dz) * RAD2DEG;
  if (angle < 0) angle += 360.0f;
  g_npcs[npcIdx].facingAngle = angle;
  g_npcs[npcIdx].targetAngle = angle;
}

// ═══════════════════════════════════════════════════════════════════════════════
// MENU OVERLAY — Desert Theme Collection Browser & Deckbuilder
// ═══════════════════════════════════════════════════════════════════════════════
static void UpdateMenu() {
  // Tab switching
  if (IsKeyPressed(KEY_TAB) || IsKeyPressed(KEY_RIGHT))
    g_menuTab = (MenuTab)((g_menuTab + 1) % TAB_COUNT);
  if (IsKeyPressed(KEY_LEFT) && g_menuTab > 0)
    g_menuTab = (MenuTab)(g_menuTab - 1);

  // Scrolling
  int wheel = (int)GetMouseWheelMove();
  if (g_menuTab == TAB_COLLECTION) {
    g_collScroll -= wheel;
    if (g_collScroll < 0)
      g_collScroll = 0;
    int maxScroll = (g_numCardCopies / 3) - 3;
    if (maxScroll < 0)
      maxScroll = 0;
    if (g_collScroll > maxScroll)
      g_collScroll = maxScroll;
  } else {
    g_deckScroll -= wheel;
    if (g_deckScroll < 0)
      g_deckScroll = 0;
  }

  // Close menu
  if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
    g_menuOpen = false;
    return;
  }
}

static void DrawMenuOverlay() {
  if (!g_menuOpen)
    return;
  RebuildCardCopies();
  UpdateMenu();

  // Semi-transparent dark overlay
  DrawRectangle(0, 0, SCREEN_W, SCREEN_H, {20, 15, 8, 200});

  // Menu frame
  int mx = 30, my = 20, mw = SCREEN_W - 60, mh = SCREEN_H - 40;
  DrawRectangleRounded({(float)mx, (float)my, (float)mw, (float)mh}, 0.02f, 4,
                       {45, 35, 20, 250});
  DrawRectangleRoundedLines({(float)mx, (float)my, (float)mw, (float)mh}, 0.02f,
                            4, {210, 175, 90, 255});
  DrawRectangleRoundedLines(
      {(float)(mx + 2), (float)(my + 2), (float)(mw - 4), (float)(mh - 4)},
      0.02f, 4, {140, 110, 60, 180});

  // Title bar
  DrawRectangle(mx + 4, my + 4, mw - 8, 28, {60, 45, 25, 255});
  DrawText("SOVEREIGN HORIZONS", mx + mw / 2 - 80, my + 10, 16,
           {220, 190, 100, 255});

  // Tab buttons
  int tabY = my + 36;
  const char *tabNames[] = {"Collection", "Deck Builder", "Save/Load",
                            "Settings"};
  int tabWidths[] = {100, 110, 90, 80};
  int tabCumX = mx + 10;
  for (int t = 0; t < TAB_COUNT; t++) {
    bool active = (g_menuTab == (MenuTab)t);
    Color bg = active ? Color{110, 85, 50, 255} : Color{60, 45, 25, 255};
    Color fg = active ? Color{240, 220, 150, 255} : Color{160, 140, 100, 255};
    DrawRectangle(tabCumX, tabY, tabWidths[t], 22, bg);
    DrawRectangleLines(tabCumX, tabY, tabWidths[t], 22, {140, 110, 60, 200});
    DrawText(tabNames[t], tabCumX + 8, tabY + 5, 12, fg);
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
      Vector2 mp = GetMousePosition();
      if (mp.x >= tabCumX && mp.x < tabCumX + tabWidths[t] && mp.y >= tabY &&
          mp.y < tabY + 22)
        g_menuTab = (MenuTab)t;
    }
    tabCumX += tabWidths[t] + 6;
  }

  int panelY = tabY + 30;
  int panelH = mh - (panelY - my) - 10;

  if (g_menuTab == TAB_COLLECTION) {
    // COLLECTION BROWSER: 3x4 grid of card thumbnails
    DrawText("Your Collection", mx + 15, panelY, 14, {200, 175, 100, 255});

    int cardW = 100, cardH = 136;
    int gapX = 12, gapY = 10;
    int gridX = mx + 15, gridY = panelY + 22;
    int cols = 3, rows = 4;

    // Left panel: card grid
    for (int row = 0; row < rows; row++) {
      for (int col = 0; col < cols; col++) {
        int idx = (g_collScroll + row) * cols + col;
        if (idx >= g_numCardCopies)
          continue;

        int cx2 = gridX + col * (cardW + gapX);
        int cy2 = gridY + row * (cardH + gapY);
        int cid = g_cardCopies[idx].cardId;

        // Draw card texture
        if (cid > 0 && cid < 200 && g_cardTextures[cid].id) {
          Rectangle src = {0, 0, (float)CARD_TEX_W, (float)CARD_TEX_H};
          Rectangle dst = {(float)cx2, (float)cy2, (float)cardW, (float)cardH};
          DrawTexturePro(g_cardTextures[cid], src, dst, {0, 0}, 0, WHITE);
        }

        // Highlight selected
        if (g_selectedCardId == cid) {
          DrawRectangleLinesEx({(float)cx2 - 1, (float)cy2 - 1,
                                (float)(cardW + 2), (float)(cardH + 2)},
                               2, {255, 220, 80, 255});
        }

        // Copy count below card
        char countStr[16];
        snprintf(countStr, 16, "x%d", g_cardCopies[idx].count);
        int tw = MeasureText(countStr, 10);
        DrawText(countStr, cx2 + cardW / 2 - tw / 2, cy2 + cardH + 2, 10,
                 {200, 175, 100, 200});

        // Click to select
        Vector2 mp = GetMousePosition();
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && mp.x >= cx2 &&
            mp.x < cx2 + cardW && mp.y >= cy2 && mp.y < cy2 + cardH) {
          g_selectedCardId = cid;
        }
      }
    }

    // Right panel: card detail
    int detX = gridX + cols * (cardW + gapX) + 20;
    int detW = mx + mw - 10 - detX;
    DrawRectangle(detX, gridY, detW, panelH - 30, {35, 28, 18, 220});
    DrawRectangleLines(detX, gridY, detW, panelH - 30, {140, 110, 60, 180});

    if (g_selectedCardId > 0) {
      const CardDef &cd = GetCard(g_selectedCardId);
      // Large card preview
      if (g_cardTextures[g_selectedCardId].id) {
        Rectangle src = {0, 0, (float)CARD_TEX_W, (float)CARD_TEX_H};
        Rectangle dst = {(float)(detX + detW / 2 - 72), (float)(gridY + 8), 144,
                         192};
        DrawTexturePro(g_cardTextures[g_selectedCardId], src, dst, {0, 0}, 0,
                       WHITE);
      }
      // Card details below
      int ty = gridY + 208;
      DrawText(cd.name, detX + 10, ty, 14, {240, 220, 150, 255});
      ty += 20;
      char info[64];
      if (cd.isUnit) {
        snprintf(info, 64, "Cost: %d  ATK: %d  DEF: %d", cd.cost, cd.atk,
                 cd.def);
      } else {
        snprintf(info, 64, "Support - Cost: %d", cd.cost);
      }
      DrawText(info, detX + 10, ty, 11, {180, 160, 110, 255});
      ty += 16;
      if (cd.isUnit && cd.subtype[0]) {
        DrawText(cd.subtype, detX + 10, ty, 10, {160, 140, 100, 200});
        ty += 14;
      }
      if (cd.keywords[0]) {
        DrawText(cd.keywords, detX + 10, ty, 10, {200, 180, 80, 220});
        ty += 14;
      }
      if (cd.effect[0]) {
        DrawText(cd.effect, detX + 10, ty, 10, {180, 170, 130, 220});
        ty += 14;
      }

      // "Add to Deck" button
      int btnY = ty + 10;
      DrawRectangle(detX + 10, btnY, 100, 22, {80, 60, 35, 255});
      DrawRectangleLines(detX + 10, btnY, 100, 22, {180, 150, 80, 200});
      DrawText("[A] Add to Deck", detX + 14, btnY + 5, 10,
               {220, 200, 120, 255});

      // Handle add-to-deck
      if (IsKeyPressed(KEY_A) ||
          (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
           GetMousePosition().x >= detX + 10 &&
           GetMousePosition().x < detX + 110 && GetMousePosition().y >= btnY &&
           GetMousePosition().y < btnY + 22)) {
        if (g_playerDeckSize < MAX_DECK) {
          g_playerDeck[g_playerDeckSize++] = g_selectedCardId;
        }
      }
    }

    DrawText("Scroll: Mouse Wheel", mx + 15, my + mh - 20, 10,
             {140, 120, 80, 150});

  } else if (g_menuTab == TAB_DECKS) {
    // DECK BUILDER: Split screen
    int leftW = mw / 2 - 20;
    DrawRectangle(mx + 10, panelY, leftW, panelH - 10, {35, 28, 18, 220});
    DrawRectangleLines(mx + 10, panelY, leftW, panelH - 10,
                       {140, 110, 60, 180});

    char deckTitle[64];
    snprintf(deckTitle, 64, "Your Deck (%d/%d cards)", g_playerDeckSize,
             MAX_DECK);
    DrawText(deckTitle, mx + 18, panelY + 6, 13, {220, 190, 100, 255});

    // List deck cards
    int listY = panelY + 24;
    int itemH = 18;
    int visible = (panelH - 40) / itemH;
    for (int i = g_deckScroll;
         i < g_playerDeckSize && (i - g_deckScroll) < visible; i++) {
      int iy = listY + (i - g_deckScroll) * itemH;
      const CardDef &cd = GetCard(g_playerDeck[i]);
      bool hovered = false;
      Vector2 mp = GetMousePosition();
      if (mp.x >= mx + 12 && mp.x < mx + 10 + leftW - 4 && mp.y >= iy &&
          mp.y < iy + itemH) {
        hovered = true;
        DrawRectangle(mx + 12, iy, leftW - 4, itemH, {60, 45, 25, 180});
      }
      Color tc =
          cd.isUnit ? Color{200, 180, 120, 255} : Color{120, 180, 120, 255};
      char entry[80];
      snprintf(entry, 80, "%d. [%d] %s", i + 1, cd.cost, cd.name);
      DrawText(entry, mx + 18, iy + 3, 10, tc);

      // Click to remove from deck
      if (hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        for (int j = i; j < g_playerDeckSize - 1; j++)
          g_playerDeck[j] = g_playerDeck[j + 1];
        g_playerDeckSize--;
        if (g_deckScroll > 0 && g_deckScroll >= g_playerDeckSize)
          g_deckScroll--;
      }
    }

    // Remove hint
    DrawText("Click card to remove  |  [B] Remove last", mx + 18,
             panelY + panelH - 22, 9, {160, 140, 100, 160});
    if (IsKeyPressed(KEY_B) && g_playerDeckSize > 0)
      g_playerDeckSize--;

    // Right: Card pool (collection browser)
    int rightX = mx + 10 + leftW + 10;
    int rightW = mw - leftW - 30;
    DrawRectangle(rightX, panelY, rightW, panelH - 10, {35, 28, 18, 220});
    DrawRectangleLines(rightX, panelY, rightW, panelH - 10,
                       {140, 110, 60, 180});
    DrawText("Card Pool", rightX + 8, panelY + 6, 13, {220, 190, 100, 255});

    // Compact card list (pool)
    int poolY = panelY + 24;
    int poolVisible = (panelH - 40) / itemH;
    for (int i = 0; i < g_numCardCopies && i < poolVisible; i++) {
      int iy = poolY + i * itemH;
      const CardDef &cd = GetCard(g_cardCopies[i].cardId);
      bool hovered = false;
      Vector2 mp = GetMousePosition();
      if (mp.x >= rightX + 4 && mp.x < rightX + rightW - 4 && mp.y >= iy &&
          mp.y < iy + itemH) {
        hovered = true;
        DrawRectangle(rightX + 4, iy, rightW - 8, itemH, {60, 45, 25, 180});
        if (cd.effect[0]) {
          DrawRectangle((int)mp.x + 10, (int)mp.y - 30, 200, 28,
                        {30, 25, 15, 240});
          DrawRectangleLines((int)mp.x + 10, (int)mp.y - 30, 200, 28,
                             {140, 110, 60, 200});
          DrawText(cd.effect, (int)mp.x + 14, (int)mp.y - 26, 8,
                   {200, 180, 130, 255});
        }
      }
      Color tc =
          cd.isUnit ? Color{200, 180, 120, 255} : Color{120, 180, 120, 255};
      char entry[80];
      if (cd.isUnit)
        snprintf(entry, 80, "[%d] %s %d/%d x%d", cd.cost, cd.name, cd.atk,
                 cd.def, g_cardCopies[i].count);
      else
        snprintf(entry, 80, "[%d] %s x%d", cd.cost, cd.name,
                 g_cardCopies[i].count);
      DrawText(entry, rightX + 10, iy + 3, 10, tc);

      // Click to add to deck
      if (hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        if (g_playerDeckSize < MAX_DECK) {
          g_playerDeck[g_playerDeckSize++] = g_cardCopies[i].cardId;
        }
      }
    }

  } else if (g_menuTab == TAB_SAVE) {
    // SAVE/LOAD SYSTEM
    DrawText("Save & Load", mx + 15, panelY, 16, {220, 190, 100, 255});

    int btnW = 200, btnH = 40;
    int cx = mx + mw / 2 - btnW / 2;

    // Save button
    DrawRectangleRounded(
        {(float)cx, (float)(panelY + 40), (float)btnW, (float)btnH}, 0.3f, 4,
        {60, 120, 60, 255});
    DrawRectangleLinesEx(
        {(float)cx, (float)(panelY + 40), (float)btnW, (float)btnH}, 1.0f,
        {100, 180, 100, 255});
    DrawText("SAVE GAME", cx + 55, panelY + 52, 14, {220, 255, 220, 255});
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
      Vector2 mp = GetMousePosition();
      if (mp.x >= cx && mp.x < cx + btnW && mp.y >= panelY + 40 &&
          mp.y < panelY + 40 + btnH) {
        if (SaveGame()) {
          g_saveNotification = true;
          g_saveNotifyTimer = 2.0f;
        }
      }
    }

    // Load button
    DrawRectangleRounded(
        {(float)cx, (float)(panelY + 100), (float)btnW, (float)btnH}, 0.3f, 4,
        {60, 80, 140, 255});
    DrawRectangleLinesEx(
        {(float)cx, (float)(panelY + 100), (float)btnW, (float)btnH}, 1.0f,
        {100, 140, 220, 255});
    DrawText("LOAD GAME", cx + 55, panelY + 112, 14, {220, 220, 255, 255});
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
      Vector2 mp = GetMousePosition();
      if (mp.x >= cx && mp.x < cx + btnW && mp.y >= panelY + 100 &&
          mp.y < panelY + 100 + btnH) {
        if (LoadGame()) {
          g_menuOpen = false;
        }
      }
    }

    // Save notification
    if (g_saveNotification) {
      DrawText("Game Saved!", cx + 55, panelY + 160, 14, {120, 255, 120, 255});
    }

    DrawText("Save file: savegame.dat (in game directory)", mx + 15,
             panelY + panelH - 30, 10, {140, 120, 80, 150});

  } else if (g_menuTab == TAB_SETTINGS) {
    // SETTINGS
    DrawText("Settings", mx + 15, panelY, 16, {220, 190, 100, 255});

    // Tournament info
    char tInfo[128];
    snprintf(tInfo, 128, "City: %s", g_tournament.GetCityName());
    DrawText(tInfo, mx + 15, panelY + 40, 12, {200, 180, 130, 255});
    snprintf(tInfo, 128, "League: %s  (Round %d/%d)",
             g_tournament.GetLeagueName(), g_tournament.roundsWon,
             g_tournament.roundsPerLeague);
    DrawText(tInfo, mx + 15, panelY + 58, 12, {200, 180, 130, 255});
    snprintf(tInfo, 128, "Tournaments Won: %d",
             g_tournament.totalTournamentsWon);
    DrawText(tInfo, mx + 15, panelY + 76, 12, {200, 180, 130, 255});
    if (g_tournament.capitalUnlocked)
      DrawText("CAPITAL UNLOCKED!", mx + 15, panelY + 94, 14,
               {255, 220, 80, 255});

    // Inventory
    DrawText("Inventory:", mx + 15, panelY + 120, 14, {220, 190, 100, 255});
    for (int i = 0; i < ITEM_COUNT; i++) {
      Color c = g_inventory.items[i].owned ? Color{200, 255, 200, 255}
                                           : Color{100, 90, 70, 150};
      snprintf(tInfo, 128, "%s %s - %s",
               g_inventory.items[i].owned ? "[x]" : "[ ]",
               g_inventory.items[i].name, g_inventory.items[i].desc);
      DrawText(tInfo, mx + 25, panelY + 140 + i * 18, 10, c);
    }

    // Market info
    DrawText("Economy:", mx + 15, panelY + 250, 14, {220, 190, 100, 255});
    snprintf(tInfo, 128, "Time: %s  |  Shop Modifier: %.0f%%",
             g_worldClock.GetName(), g_worldClock.GetShopMod() * 100);
    DrawText(tInfo, mx + 25, panelY + 270, 10, {180, 160, 120, 200});
  }

  // Bottom status bar
  char statusStr[128];
  snprintf(statusStr, 128,
           "Coins: %d  |  Collection: %d cards  |  Deck: %d/%d  |  Tab/Arrows: "
           "Switch  |  Esc: Close",
           g_playerCoins, g_collectionSize, g_playerDeckSize, MAX_DECK);
  DrawText(statusStr, mx + 15, my + mh - 8, 9, {160, 140, 100, 180});

  // Save notification overlay (decays)
  if (g_saveNotification) {
    g_saveNotifyTimer -= GetFrameTime();
    if (g_saveNotifyTimer <= 0) {
      g_saveNotification = false;
      g_saveNotifyTimer = 0;
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// HD-2D SPRITE GENERATION — Pokémon BW-style volumetric pixel art (24×32)
// 3-tone shading per element, 1px outline, dynamic walk cycle
// ═══════════════════════════════════════════════════════════════════════════════
static const Color BK = BLANK;
static void Px(Image *img, int x, int y, Color c) {
  if (x >= 0 && x < SPR_W && y >= 0 && y < SPR_H && c.a > 0)
    ImageDrawPixel(img, x, y, c);
}
static void PxR(Image *img, int x, int y, int w, int h, Color c) {
  for (int py = y; py < y + h; py++)
    for (int px = x; px < x + w; px++)
      Px(img, px, py, c);
}
// Outline: scan all pixels, add dark border around non-transparent
static void AddOutline(Image *img, Color ol) {
  Image tmp = ImageCopy(*img);
  for (int y = 0; y < SPR_H; y++)
    for (int x = 0; x < SPR_W; x++) {
      Color c = GetImageColor(tmp, x, y);
      if (c.a < 10) {
        // Check neighbors — if any neighbor is opaque, draw outline
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0)
              continue;
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && nx < SPR_W && ny >= 0 && ny < SPR_H) {
              Color nc = GetImageColor(tmp, nx, ny);
              if (nc.a > 128) {
                Px(img, x, y, ol);
                goto done;
              }
            }
          }
      done:;
      }
    }
  UnloadImage(tmp);
}
// Color math helpers
static Color Cblend(Color a, Color b, float t) {
  return {(unsigned char)(a.r + (b.r - a.r) * t),
          (unsigned char)(a.g + (b.g - a.g) * t),
          (unsigned char)(a.b + (b.b - a.b) * t), 255};
}
static Color Cdark(Color c, float f) {
  return {(unsigned char)(c.r * f), (unsigned char)(c.g * f),
          (unsigned char)(c.b * f), 255};
}
static Color Clight(Color c, int d) {
  return {(unsigned char)Clamp(c.r + d, 0, 255),
          (unsigned char)Clamp(c.g + d, 0, 255),
          (unsigned char)Clamp(c.b + d, 0, 255), 255};
}

// ═══════════════════════════════════════════════════════════════════════════════
// 3D CHIBI MODEL SYSTEM — BDSP "Vinyl Collectible Figure" aesthetic
// Procedurally drawn using raylib primitives (DrawSphere, DrawCylinder, DrawCube)
// 2-Head-High proportions: head ≈ torso size, glossy toon shader simulation
// ═══════════════════════════════════════════════════════════════════════════════

// Dir → angle mapping: DIR_DOWN=0° (south/toward camera), UP=180°, LEFT=90°, RIGHT=270°
static float DirToAngle(Dir d) {
  switch (d) {
    case DIR_DOWN:  return 0.0f;
    case DIR_UP:    return 180.0f;
    case DIR_LEFT:  return 90.0f;
    case DIR_RIGHT: return 270.0f;
    default:        return 0.0f;
  }
}

// Smooth shortest-arc angle interpolation (handles 0↔360 wrap)
static float LerpAngle(float from, float to, float t) {
  float diff = fmodf(to - from + 540.0f, 360.0f) - 180.0f;
  return from + diff * t;
}

// Update facing angle toward target with smooth 0.15s interpolation
static void UpdateFacingAngle(float &current, float target, float dt) {
  float speed = dt / ROTATION_TIME;
  current = LerpAngle(current, target, Clamp(speed, 0.0f, 1.0f));
}

// ── Draw a single 3D chibi character ────────────────────────────────────────
// All geometry is relative to basePos (feet on ground). facingDeg rotates around Y.
// walkPhase: 0.0 = idle, else oscillating limb swing value
static void DrawChibiModel3D(Vector3 basePos, float facingDeg,
                              const ChibiColors &c, float walkPhase,
                              bool isWalking, float rimFactor) {
  // ── Coordinate system: Y is up. facingDeg rotates the model around Y.
  // The model's face is built on local -Z, so we add 180° to align
  // facingDeg=0° (south/toward camera) with the model's front facing +Z.
  rlPushMatrix();
  rlTranslatef(basePos.x, basePos.y, basePos.z);
  rlRotatef(facingDeg + 180.0f, 0.0f, 1.0f, 0.0f);

  // Walk bob: subtle vertical bounce when walking
  float bob = isWalking ? fabsf(sinf(walkPhase * 12.0f)) * 0.03f : 0.0f;

  // ── CHUNKY BOOTS (y: 0.0 → 0.18) — vinyl figure base ──────────────────
  float bootY = 0.09f + bob;
  float legSwing = isWalking ? sinf(walkPhase * 10.0f) * 0.12f : 0.0f;
  // Left boot
  DrawCube({-0.10f, bootY, legSwing * 0.3f}, 0.14f, 0.18f, 0.18f, c.shoe);
  DrawCube({-0.10f, bootY + 0.04f, legSwing * 0.3f}, 0.12f, 0.06f, 0.14f, c.shoeHi); // gloss strip
  // Right boot
  DrawCube({0.10f, bootY, -legSwing * 0.3f}, 0.14f, 0.18f, 0.18f, c.shoe);
  DrawCube({0.10f, bootY + 0.04f, -legSwing * 0.3f}, 0.12f, 0.06f, 0.14f, c.shoeHi);

  // ── STOCKY LEGS (y: 0.18 → 0.34) ──────────────────────────────────────
  float legY = 0.26f + bob;
  DrawCylinder({-0.08f, 0.18f + bob, legSwing * 0.2f}, 0.055f, 0.065f, 0.16f, 6, c.pants);
  DrawCylinder({0.08f, 0.18f + bob, -legSwing * 0.2f}, 0.055f, 0.065f, 0.16f, 6, c.pantsSh);

  // ── PEAR-SHAPED TORSO (y: 0.34 → 0.62) — compact jacket ──────────────
  float torsoY = 0.48f + bob;
  // Main jacket body (wider at hips, narrower at shoulders)
  DrawCube({0.0f, 0.40f + bob, 0.0f}, 0.30f, 0.12f, 0.22f, c.jacket);    // hip
  DrawCube({0.0f, 0.48f + bob, 0.0f}, 0.26f, 0.10f, 0.20f, c.jacket);    // mid
  DrawCube({0.0f, 0.55f + bob, 0.0f}, 0.22f, 0.08f, 0.18f, c.jacketHi);  // shoulders
  // Jacket shadow (left side darker)
  DrawCube({-0.12f, 0.44f + bob, 0.0f}, 0.06f, 0.18f, 0.18f, c.jacketSh);
  // Vinyl sheen highlight (right shoulder area)
  DrawCube({0.08f, 0.54f + bob, -0.06f}, 0.04f, 0.04f, 0.04f, c.jacketHi);

  // ── MITTEN ARMS (stubby cylinders with thumb nub) ─────────────────────
  float armSwing = isWalking ? sinf(walkPhase * 10.0f) * 0.15f : 0.0f;
  // Left arm
  DrawCylinder({-0.18f, 0.42f + bob, armSwing * 0.4f}, 0.045f, 0.04f, 0.18f, 6, c.jacketSh);
  // Left mitten hand
  DrawSphere({-0.18f, 0.38f + bob, armSwing * 0.5f}, 0.045f, c.skin);
  DrawSphere({-0.21f, 0.39f + bob, armSwing * 0.5f}, 0.02f, c.skinSh); // thumb
  // Right arm
  DrawCylinder({0.18f, 0.42f + bob, -armSwing * 0.4f}, 0.045f, 0.04f, 0.18f, 6, c.jacketHi);
  // Right mitten hand
  DrawSphere({0.18f, 0.38f + bob, -armSwing * 0.5f}, 0.045f, c.skin);
  DrawSphere({0.21f, 0.39f + bob, -armSwing * 0.5f}, 0.02f, c.skinSh); // thumb

  // ── NECK (tiny connector) ─────────────────────────────────────────────
  float neckY = 0.60f + bob;
  DrawCylinder({0.0f, neckY, 0.0f}, 0.04f, 0.05f, 0.04f, 6, c.skinSh);

  // ── HUGE SPHERICAL HEAD (y: 0.64 → 1.14) — 2-head-high vinyl dome ────
  float headY = 0.88f + bob;
  float headR = 0.25f;
  // Main head sphere (smooth plastic)
  DrawSphere({0.0f, headY, 0.0f}, headR, c.skin);
  // Forehead highlight (vinyl SSS glow — slightly offset sphere)
  DrawSphere({0.0f, headY + 0.06f, -0.08f}, headR * 0.35f, c.skinHi);

  // ── SCULPTED HAIR MASS — chunky molded plastic ────────────────────────
  // Top dome
  DrawSphere({0.0f, headY + 0.16f, -0.02f}, 0.20f, c.hair);
  // Left tuft
  DrawSphere({-0.18f, headY + 0.06f, -0.04f}, 0.10f, c.hair);
  // Right tuft
  DrawSphere({0.18f, headY + 0.06f, -0.04f}, 0.10f, c.hair);
  // Back hair volume
  DrawSphere({0.0f, headY + 0.04f, 0.14f}, 0.18f, c.hair);
  // Gloss streaks (vinyl sheen)
  DrawSphere({-0.06f, headY + 0.22f, -0.06f}, 0.05f, c.hairHi);
  DrawSphere({0.08f, headY + 0.18f, -0.05f}, 0.04f, c.hairHi);

  // ── CAP / HAT (sits atop the hair) ────────────────────────────────────
  DrawSphere({0.0f, headY + 0.20f, -0.03f}, 0.22f, c.cap);
  // Cap brim (front-facing visor)
  DrawCube({0.0f, headY + 0.12f, -0.22f}, 0.30f, 0.04f, 0.12f, c.cap);
  // Cap gloss highlight
  DrawSphere({0.04f, headY + 0.26f, -0.06f}, 0.05f, c.capHi);

  // ── FACE FEATURES (on the front = -Z side of head in local space) ─────
  // Eyes: two glossy orbs with iris/pupil layering
  float eyeY = headY - 0.02f;
  float eyeZ = -0.20f; // front face
  // Left eye white (sclera)
  DrawSphere({-0.08f, eyeY, eyeZ}, 0.055f, WHITE);
  // Left iris
  DrawSphere({-0.08f, eyeY, eyeZ - 0.03f}, 0.04f, c.eyeIris);
  // Left pupil
  DrawSphere({-0.08f, eyeY - 0.005f, eyeZ - 0.045f}, 0.025f, c.eye);
  // Left sparkle (glossy vinyl highlight)
  DrawSphere({-0.06f, eyeY + 0.02f, eyeZ - 0.05f}, 0.012f, WHITE);

  // Right eye white
  DrawSphere({0.08f, eyeY, eyeZ}, 0.055f, WHITE);
  // Right iris
  DrawSphere({0.08f, eyeY, eyeZ - 0.03f}, 0.04f, c.eyeIris);
  // Right pupil
  DrawSphere({0.08f, eyeY - 0.005f, eyeZ - 0.045f}, 0.025f, c.eye);
  // Right sparkle
  DrawSphere({0.10f, eyeY + 0.02f, eyeZ - 0.05f}, 0.012f, WHITE);

  // Blush spots (subtle pink on cheeks)
  DrawSphere({-0.14f, eyeY - 0.06f, eyeZ + 0.04f}, 0.035f, {255, 160, 160, 80});
  DrawSphere({0.14f, eyeY - 0.06f, eyeZ + 0.04f}, 0.035f, {255, 160, 160, 80});

  // Tiny nose
  DrawSphere({0.0f, eyeY - 0.05f, eyeZ - 0.02f}, 0.015f, c.skinSh);

  // Mouth (small line — drawn as tiny dark cube)
  DrawCube({0.0f, eyeY - 0.10f, eyeZ}, 0.04f, 0.008f, 0.01f, c.skinSh);

  // ── RIM LIGHT (simulated by a bright hemisphere on the back-right) ────
  if (rimFactor > 0.0f) {
    Color rim = {255, 240, 220, (unsigned char)(rimFactor * 60)};
    DrawSphere({0.10f, headY + 0.05f, 0.18f}, 0.08f, rim);   // head rim
    DrawSphere({0.12f, 0.48f + bob, 0.10f}, 0.05f, rim);      // torso rim
  }

  rlPopMatrix();
}

// ── Helper: convert legacy CharPalette-style NPC colors to ChibiColors ──────
static ChibiColors MakeChibiColors(Color skin, Color hair, Color cap,
                                     Color jacket, Color pants, Color shoe) {
  ChibiColors cc;
  cc.skin    = skin;
  cc.skinHi  = Clight(skin, 25);
  cc.skinSh  = Cdark(skin, 0.82f);
  cc.hair    = hair;
  cc.hairHi  = Clight(hair, 40);
  cc.eye     = {20, 20, 30, 255};
  cc.eyeIris = {65, 90, 160, 255};
  cc.cap     = cap;
  cc.capHi   = Clight(cap, 35);
  cc.jacket    = jacket;
  cc.jacketHi  = Clight(jacket, 25);
  cc.jacketSh  = Cdark(jacket, 0.75f);
  cc.pants   = pants;
  cc.pantsSh = Cdark(pants, 0.8f);
  cc.shoe    = shoe;
  cc.shoeHi  = Clight(shoe, 30);
  cc.outline = {25, 20, 18, 255};
  return cc;
}

// Player 3D chibi colors (initialized in InitOverworld)
// Kept as a function to avoid static init order issues with Color literals

// ═══════════════════════════════════════════════════════════════════════════════
// [LEGACY SPRITE GENERATION REMOVED — replaced by DrawChibiModel3D above]
// Keeping pixel art helpers (Px, PxR, AddOutline, Cdark, Clight, Cblend)
// for any future UI/icon use.
// ═══════════════════════════════════════════════════════════════════════════════

// [DELETED: GenChibiDown — replaced by DrawChibiModel3D]
#if 0 // ── DEAD CODE: old 2D sprite generators ──
static Image _dead_GenChibiDown(int f, int _dummy) {
  Image img = GenImageColor(SPR_W, SPR_H, BK);
  int lo = (f == 1) ? 1 : (f == 2) ? -1 : 0;

  // ── SCULPTED CAP DOME (rows 0-7) — glossy vinyl sphere ──
  PxR(&img, 8, 0, 8, 1, p.capFront);                       // dome peak
  PxR(&img, 6, 1, 12, 1, p.capFront);
  Px(&img, 8, 0, p.capGloss);                               // specular hotspot
  Px(&img, 9, 0, p.capGloss);
  Px(&img, 8, 1, Clight(p.capFront, 40));                   // secondary gloss
  PxR(&img, 4, 2, 16, 2, p.capFront);
  PxR(&img, 13, 2, 7, 2, Cdark(p.capBack, 0.92f));
  Px(&img, 7, 2, Clight(p.capFront, 25));                   // rim light
  PxR(&img, 3, 4, 18, 2, p.capFront);
  PxR(&img, 14, 4, 7, 2, Cdark(p.capBack, 0.88f));
  PxR(&img, 3, 6, 18, 2, p.capBrim);                        // thick brim
  Px(&img, 5, 6, Clight(p.capBrim, 30));                    // brim gloss

  // ── THICK SCULPTED HAIR (vinyl toy masses) ─────────────
  PxR(&img, 2, 4, 2, 5, p.hair);                            // left hair mass
  PxR(&img, 20, 4, 2, 5, p.hair);                           // right hair mass
  Px(&img, 3, 4, p.hairHi);                                 // left gloss streak
  Px(&img, 21, 4, p.hairHi);                                // right gloss streak
  PxR(&img, 2, 8, 3, 2, p.hair);                            // left lower tuft
  PxR(&img, 19, 8, 3, 2, p.hair);                           // right lower tuft
  Px(&img, 3, 8, p.hairHi);
  Px(&img, 20, 8, p.hairHi);
  PxR(&img, 4, 8, 2, 1, Cdark(p.hair, 0.85f));             // fringe peek under brim
  PxR(&img, 18, 8, 2, 1, Cdark(p.hair, 0.85f));

  // ── HUGE ROUND FACE (rows 8-18) — vinyl figure proportions ──
  PxR(&img, 5, 8, 14, 1, p.skinHi);                         // forehead top
  PxR(&img, 4, 9, 16, 1, p.skinHi);                         // wider forehead
  PxR(&img, 3, 10, 18, 5, p.skin);                          // main face (widest)
  PxR(&img, 4, 15, 16, 1, p.skin);                          // chin row 1
  PxR(&img, 5, 16, 14, 1, p.skinSh);                        // chin row 2
  PxR(&img, 7, 17, 10, 1, p.skinSh);                        // chin row 3
  PxR(&img, 9, 18, 6, 1, Cdark(p.skinSh, 0.92f));          // chin bottom
  // Face shading — vinyl SSS glow
  PxR(&img, 3, 10, 1, 5, p.skinSh);                         // left shadow
  PxR(&img, 20, 10, 1, 5, Clight(p.skinHi, 15));           // right rim light
  Px(&img, 6, 9, Clight(p.skinHi, 20));                     // forehead gloss

  // ── HUGE GLOSSY EYES (rows 10-14) — 5×4 with iris gradient + double sparkle
  // Left eye sclera
  PxR(&img, 5, 10, 5, 4, WHITE);
  // Right eye sclera
  PxR(&img, 14, 10, 5, 4, WHITE);
  // Left iris (3 col gradient: light top → iris mid → pupil bottom)
  PxR(&img, 6, 10, 3, 1, Clight(p.eyeIris, 30));           // iris top (bright)
  PxR(&img, 6, 11, 3, 2, p.eyeIris);                        // iris middle
  PxR(&img, 6, 13, 3, 1, p.eye);                            // pupil row
  // Right iris
  PxR(&img, 15, 10, 3, 1, Clight(p.eyeIris, 30));
  PxR(&img, 15, 11, 3, 2, p.eyeIris);
  PxR(&img, 15, 13, 3, 1, p.eye);
  // Pupil cores
  Px(&img, 7, 12, Cdark(p.eye, 0.5f));
  Px(&img, 16, 12, Cdark(p.eye, 0.5f));
  // Double sparkle (vinyl glossy eyes)
  Px(&img, 8, 10, {255, 255, 255, 255});                    // big sparkle UL
  Px(&img, 17, 10, {255, 255, 255, 255});
  Px(&img, 6, 12, {255, 255, 255, 200});                    // small sparkle LR
  Px(&img, 15, 12, {255, 255, 255, 200});
  // Eyelash line (top)
  PxR(&img, 5, 10, 1, 1, p.outline);
  PxR(&img, 9, 10, 1, 1, p.outline);
  PxR(&img, 14, 10, 1, 1, p.outline);
  PxR(&img, 18, 10, 1, 1, p.outline);

  // Blush ovals (larger, more prominent for vinyl look)
  PxR(&img, 3, 14, 3, 1, {255, 160, 160, 70});             // left cheek
  PxR(&img, 18, 14, 3, 1, {255, 160, 160, 70});            // right cheek
  // Tiny nose
  Px(&img, 12, 14, p.skinSh);
  // Small :3 mouth
  Px(&img, 11, 16, Cdark(p.skinSh, 0.9f));
  Px(&img, 12, 16, Cdark(p.skinSh, 0.9f));

  // ── Neck (short, stubby) ──────────────────────────
  PxR(&img, 10, 19, 4, 1, p.skinSh);

  // ── COMPACT PEAR-SHAPED TORSO (rows 20-25) — vinyl jacket ──
  PxR(&img, 8, 20, 8, 6, p.jacket);                         // base jacket
  PxR(&img, 8, 20, 8, 1, p.jacketHi);                       // collar highlight
  PxR(&img, 8, 20, 2, 6, p.jacketSh);                       // left shadow
  PxR(&img, 14, 20, 2, 6, p.jacketHi);                      // right highlight
  Px(&img, 13, 21, p.jacketGloss);                          // vinyl sheen spot
  Px(&img, 13, 22, p.jacketGloss);
  PxR(&img, 10, 22, 4, 1, p.stripe);                        // chest stripe
  PxR(&img, 10, 23, 4, 1, Cdark(p.stripe, 0.9f));
  // Mitten arms (stubby, round, animated swing)
  int laOff = (f == 1) ? -1 : 0;
  int raOff = (f == 2) ? -1 : 0;
  PxR(&img, 6, 21 + laOff, 2, 3, p.jacketSh);              // left arm
  PxR(&img, 16, 21 + raOff, 2, 3, p.jacketHi);             // right arm
  // Mitten hands (2×2 blob + thumb pixel)
  PxR(&img, 6, 23 + laOff, 2, 2, p.skin);                  // left mitten
  Px(&img, 5, 23 + laOff, p.skinSh);                        // left thumb
  PxR(&img, 16, 23 + raOff, 2, 2, p.skin);                 // right mitten
  Px(&img, 18, 23 + raOff, p.skinSh);                       // right thumb

  // ── SHORT STOCKY LEGS (rows 26-28) ─────────────────
  PxR(&img, 9, 26, 6, 3, p.pants);
  PxR(&img, 9, 26, 3, 3, p.pantsSh);
  if (f == 1) {
    PxR(&img, 9, 26, 3, 3 + lo, p.pants);
    PxR(&img, 12, 26, 3, 3 - lo, p.pantsSh);
  } else if (f == 2) {
    PxR(&img, 9, 26, 3, 3 - lo, p.pantsSh);
    PxR(&img, 12, 26, 3, 3 + lo, p.pants);
  }

  // ── CHUNKY BOOTS (rows 29-31) — vinyl figure base ──
  int sy = 29 + lo;
  PxR(&img, 7, sy, 5, 2, p.shoe);                           // left boot
  PxR(&img, 12, sy - lo * 2, 5, 2, p.shoe);                // right boot
  Px(&img, 9, sy, p.bootHi);                                // left boot gloss
  Px(&img, 14, sy - lo * 2, p.bootHi);                     // right boot gloss
  PxR(&img, 7, sy + 1, 5, 1, p.shoeSh);                    // left sole
  PxR(&img, 12, sy - lo * 2 + 1, 5, 1, p.shoeSh);         // right sole

  AddOutline(&img, p.outline);
  return img;
}

// ── Up-facing sprite (walking away) — Vinyl Figure Back ─────────────────────
static Image GenChibiUp(int f, const CharPalette &p) {
  Image img = GenImageColor(SPR_W, SPR_H, BK);
  int lo = (f == 1) ? 1 : (f == 2) ? -1 : 0;

  // ── CAP BACK (dome) ────────────────────────────────
  PxR(&img, 8, 0, 8, 1, p.capBack);
  PxR(&img, 6, 1, 12, 1, p.capBack);
  PxR(&img, 4, 2, 16, 2, Cdark(p.capBack, 0.92f));
  PxR(&img, 3, 4, 18, 2, Cdark(p.capBack, 0.88f));
  PxR(&img, 3, 6, 18, 2, p.capBrim);
  Px(&img, 9, 0, Clight(p.capBack, 30));                    // gloss

  // ── THICK SCULPTED HAIR BACK ───────────────────────
  PxR(&img, 2, 4, 2, 5, p.hair);
  PxR(&img, 20, 4, 2, 5, p.hair);
  Px(&img, 3, 5, p.hairHi);
  Px(&img, 21, 5, p.hairHi);
  // Full back-of-head hair volume (rows 8-18)
  PxR(&img, 5, 8, 14, 1, p.hair);
  PxR(&img, 4, 9, 16, 1, p.hair);
  PxR(&img, 3, 10, 18, 5, p.hair);
  Px(&img, 8, 10, p.hairHi);                                // gloss streak
  Px(&img, 8, 11, p.hairHi);
  PxR(&img, 4, 15, 16, 1, Cdark(p.hair, 0.88f));
  PxR(&img, 5, 16, 14, 1, Cdark(p.hair, 0.82f));
  PxR(&img, 7, 17, 10, 1, Cdark(p.hair, 0.75f));
  PxR(&img, 9, 18, 6, 1, Cdark(p.hair, 0.70f));

  // ── Neck ──────────────────────────────────────────
  PxR(&img, 10, 19, 4, 1, p.skinSh);

  // ── Jacket back (rows 20-25) ──────────────────────
  PxR(&img, 8, 20, 8, 6, p.jacket);
  PxR(&img, 8, 20, 8, 1, p.jacketHi);
  PxR(&img, 8, 20, 2, 6, p.jacketSh);
  PxR(&img, 14, 20, 2, 6, p.jacketHi);
  PxR(&img, 10, 22, 4, 1, Cdark(p.stripe, 0.85f));
  Px(&img, 13, 21, p.jacketGloss);
  // Arms
  int laOff = (f == 1) ? -1 : 0;
  int raOff = (f == 2) ? -1 : 0;
  PxR(&img, 6, 21 + laOff, 2, 3, p.jacketSh);
  PxR(&img, 16, 21 + raOff, 2, 3, p.jacketHi);
  PxR(&img, 6, 23 + laOff, 2, 2, p.skin);
  Px(&img, 5, 23 + laOff, p.skinSh);
  PxR(&img, 16, 23 + raOff, 2, 2, p.skin);
  Px(&img, 18, 23 + raOff, p.skinSh);

  // ── Pants ─────────────────────────────────────────
  PxR(&img, 9, 26, 6, 3, p.pants);
  PxR(&img, 9, 26, 3, 3, p.pantsSh);
  if (f == 1) {
    PxR(&img, 9, 26, 3, 3 + lo, p.pants);
    PxR(&img, 12, 26, 3, 3 - lo, p.pantsSh);
  } else if (f == 2) {
    PxR(&img, 9, 26, 3, 3 - lo, p.pantsSh);
    PxR(&img, 12, 26, 3, 3 + lo, p.pants);
  }

  // ── Chunky boots ──────────────────────────────────
  int sy = 29 + lo;
  PxR(&img, 7, sy, 5, 2, p.shoe);
  PxR(&img, 12, sy - lo * 2, 5, 2, p.shoe);
  Px(&img, 9, sy, p.bootHi);
  Px(&img, 14, sy - lo * 2, p.bootHi);
  PxR(&img, 7, sy + 1, 5, 1, p.shoeSh);
  PxR(&img, 12, sy - lo * 2 + 1, 5, 1, p.shoeSh);

  AddOutline(&img, p.outline);
  return img;
}

// ── Left-facing sprite — Vinyl Figure Side Profile ──────────────────────────
static Image GenChibiLeft(int f, const CharPalette &p) {
  Image img = GenImageColor(SPR_W, SPR_H, BK);
  int lo = (f == 1) ? 1 : (f == 2) ? -1 : 0;

  // ── CAP SIDE (dome profile) ────────────────────────
  PxR(&img, 7, 0, 8, 1, p.capFront);
  PxR(&img, 5, 1, 11, 1, p.capFront);
  PxR(&img, 4, 2, 14, 2, p.capFront);
  PxR(&img, 3, 4, 15, 2, Cdark(p.capFront, 0.88f));
  PxR(&img, 1, 6, 5, 2, p.capBrim);                         // brim sticks out left
  PxR(&img, 6, 6, 12, 2, Cdark(p.capFront, 0.82f));
  Px(&img, 9, 0, p.capGloss);                               // specular hotspot
  Px(&img, 8, 1, Clight(p.capFront, 35));

  // ── HAIR SIDE (sculpted back volume) ───────────────
  PxR(&img, 17, 2, 3, 6, p.hair);                           // back hair mass
  Px(&img, 18, 3, p.hairHi);                                // gloss streak
  Px(&img, 18, 4, p.hairHi);
  PxR(&img, 16, 8, 4, 3, p.hair);                           // lower back volume
  PxR(&img, 16, 11, 3, 2, Cdark(p.hair, 0.85f));
  PxR(&img, 4, 8, 2, 1, p.hair);                            // front tuft

  // ── HUGE SIDE FACE (rows 8-18) ─────────────────────
  PxR(&img, 5, 8, 12, 1, p.skinHi);
  PxR(&img, 4, 9, 13, 1, p.skinHi);
  PxR(&img, 3, 10, 14, 5, p.skin);
  PxR(&img, 4, 15, 13, 1, p.skin);
  PxR(&img, 5, 16, 11, 1, p.skinSh);
  PxR(&img, 7, 17, 8, 1, p.skinSh);
  PxR(&img, 9, 18, 5, 1, Cdark(p.skinSh, 0.92f));
  // Rim light
  PxR(&img, 16, 10, 1, 5, Clight(p.skinHi, 15));
  Px(&img, 6, 9, Clight(p.skinHi, 20));                     // forehead gloss

  // One HUGE eye (side view, 4×4 with iris gradient)
  PxR(&img, 5, 10, 5, 4, WHITE);
  PxR(&img, 6, 10, 3, 1, Clight(p.eyeIris, 30));
  PxR(&img, 6, 11, 3, 2, p.eyeIris);
  PxR(&img, 6, 13, 3, 1, p.eye);
  Px(&img, 7, 12, Cdark(p.eye, 0.5f));                      // pupil core
  Px(&img, 8, 10, {255, 255, 255, 255});                    // big sparkle
  Px(&img, 6, 12, {255, 255, 255, 200});                    // small sparkle
  Px(&img, 5, 10, p.outline);                               // eyelash
  Px(&img, 9, 10, p.outline);

  // Blush
  PxR(&img, 3, 14, 3, 1, {255, 160, 160, 70});
  // Profile nose (slightly protruding)
  Px(&img, 3, 12, p.skinSh);
  Px(&img, 3, 13, p.skin);
  // Mouth
  Px(&img, 6, 16, Cdark(p.skinSh, 0.9f));

  // ── Neck ──────────────────────────────────────────
  PxR(&img, 9, 19, 4, 1, p.skinSh);

  // ── Jacket side (rows 20-25) ──────────────────────
  PxR(&img, 8, 20, 7, 6, p.jacket);
  PxR(&img, 8, 20, 7, 1, p.jacketHi);
  PxR(&img, 8, 20, 2, 6, p.jacketSh);
  PxR(&img, 13, 20, 2, 6, p.jacketHi);
  Px(&img, 12, 21, p.jacketGloss);
  PxR(&img, 10, 22, 4, 1, p.stripe);
  // One visible arm (animated)
  int armOff = (f != 0) ? -1 : 0;
  PxR(&img, 6, 21 + armOff, 2, 3, p.jacketSh);
  PxR(&img, 6, 23 + armOff, 2, 2, p.skin);
  Px(&img, 5, 23 + armOff, p.skinSh);                       // thumb

  // ── Pants ─────────────────────────────────────────
  PxR(&img, 9, 26, 5, 3, p.pants);
  PxR(&img, 9, 26, 2, 3, p.pantsSh);
  if (f == 1)
    PxR(&img, 9, 26, 2, 3 + lo, p.pants);
  if (f == 2)
    PxR(&img, 9, 26, 2, 3 - lo, p.pantsSh);

  // ── Chunky boots ──────────────────────────────────
  int sy = 29 + lo;
  PxR(&img, 7, sy, 5, 2, p.shoe);
  PxR(&img, 12, sy - lo * 2, 4, 2, p.shoe);
  Px(&img, 9, sy, p.bootHi);
  PxR(&img, 7, sy + 1, 5, 1, p.shoeSh);

  AddOutline(&img, p.outline);
  return img;
}

// ── Right-facing sprite (mirror of left) ────────────────────────────────────
static Image GenChibiRight(int f, const CharPalette &p) {
  Image img = GenChibiLeft(f, p);
  ImageFlipHorizontal(&img);
  return img;
}

// ── Generate all 12 frames (4 dirs × 3 walk frames) for a palette ───────────
static void GenChibiSpriteSet(const CharPalette &pal,
                              Texture2D outFrames[TOTAL_FRAMES]) {
  for (int fr = 0; fr < FRAMES_PER_DIR; fr++) {
    Image dImg = GenChibiDown(fr, pal);
    outFrames[DIR_DOWN * FRAMES_PER_DIR + fr] = LoadTextureFromImage(dImg);
    SetTextureFilter(outFrames[DIR_DOWN * FRAMES_PER_DIR + fr],
                     TEXTURE_FILTER_POINT);
    UnloadImage(dImg);

    Image uImg = GenChibiUp(fr, pal);
    outFrames[DIR_UP * FRAMES_PER_DIR + fr] = LoadTextureFromImage(uImg);
    SetTextureFilter(outFrames[DIR_UP * FRAMES_PER_DIR + fr],
                     TEXTURE_FILTER_POINT);
    UnloadImage(uImg);

    Image lImg = GenChibiLeft(fr, pal);
    outFrames[DIR_LEFT * FRAMES_PER_DIR + fr] = LoadTextureFromImage(lImg);
    SetTextureFilter(outFrames[DIR_LEFT * FRAMES_PER_DIR + fr],
                     TEXTURE_FILTER_POINT);
    UnloadImage(lImg);

    Image rImg = GenChibiRight(fr, pal);
    outFrames[DIR_RIGHT * FRAMES_PER_DIR + fr] = LoadTextureFromImage(rImg);
    SetTextureFilter(outFrames[DIR_RIGHT * FRAMES_PER_DIR + fr],
                     TEXTURE_FILTER_POINT);
    UnloadImage(rImg);
  }
}

// ── NPC palettes (chibi villagers) ──────────────────────────────────────────
static const CharPalette g_npcPalettes[4] = {
    // NPC 0: Desert merchant — brown/gold outfit
    {{180, 140, 60, 255},
     {200, 170, 80, 255},
     {120, 90, 30, 255},
     {140, 100, 50, 255},
     {170, 130, 70, 255},
     {100, 70, 35, 255},
     {220, 200, 140, 255},
     {90, 70, 50, 255},
     {60, 45, 30, 255},
     {160, 120, 60, 255},
     {110, 80, 40, 255},
     {180, 140, 100, 255},
     {200, 165, 125, 255},
     {150, 115, 80, 255},
     {60, 40, 25, 255},
     {25, 20, 15, 255},
     {30, 22, 15, 255}},
    // NPC 1: Oasis scholar — teal/white robes
    {{60, 160, 160, 255},
     {200, 220, 220, 255},
     {40, 120, 120, 255},
     {200, 220, 220, 255},
     {230, 240, 240, 255},
     {170, 190, 190, 255},
     {60, 160, 160, 255},
     {180, 180, 170, 255},
     {140, 140, 130, 255},
     {100, 80, 60, 255},
     {70, 55, 40, 255},
     {140, 100, 70, 255},
     {165, 125, 90, 255},
     {115, 80, 55, 255},
     {30, 25, 20, 255},
     {20, 20, 25, 255},
     {25, 20, 15, 255}},
    // NPC 2: Caravan guard — dark red/black armor
    {{100, 30, 30, 255},
     {60, 20, 20, 255},
     {70, 20, 20, 255},
     {80, 30, 30, 255},
     {110, 45, 45, 255},
     {55, 20, 20, 255},
     {160, 140, 50, 255},
     {50, 45, 40, 255},
     {35, 30, 25, 255},
     {60, 50, 40, 255},
     {40, 35, 28, 255},
     {160, 120, 80, 255},
     {185, 145, 105, 255},
     {130, 95, 65, 255},
     {40, 30, 20, 255},
     {15, 15, 20, 255},
     {20, 15, 12, 255}},
    // NPC 3: Young explorer — green/tan outfit
    {{80, 140, 60, 255},
     {180, 200, 160, 255},
     {55, 100, 40, 255},
     {140, 160, 100, 255},
     {165, 185, 125, 255},
     {110, 130, 75, 255},
     {220, 210, 170, 255},
     {120, 110, 80, 255},
     {85, 78, 55, 255},
     {140, 100, 50, 255},
     {100, 70, 35, 255},
     {200, 160, 120, 255},
     {225, 185, 145, 255},
     {170, 130, 95, 255},
     {50, 35, 20, 255},
     {22, 20, 18, 255},
     {28, 22, 16, 255}}};

// ── Generate NPC sprites from their palette ─────────────────────────────────
static void GenerateNPCSprites() {
  for (int i = 0; i < 4; i++) {
    // Generate full 4-direction × 3-frame sprite set per NPC
    GenChibiSpriteSet(g_npcPalettes[i], g_npcs[i].frames);
  }
}

#endif // ── END DEAD CODE: old 2D sprite generators ──

// ── Initialize 3D chibi character colors (replaces InitCharacterSprites) ────
static void InitCharacterColors() {
  // Player: black tunic/jacket and black trousers — matte vinyl finish
  g_playerColors = MakeChibiColors(
      {222, 178, 130, 255}, // skin
      {25, 20, 18, 255},    // hair (very dark)
      {30, 28, 32, 255},    // cap (near-black)
      {22, 22, 26, 255},    // jacket (solid black, matte vinyl)
      {18, 18, 22, 255},    // pants (solid black)
      {15, 15, 18, 255}     // shoes (black)
  );
  // NPC colors initialized inline during NPC setup in InitOverworld
}

// ═══════════════════════════════════════════════════════════════════════════════
// §1  PROCEDURAL ROCK GENERATOR — Icosahedron + Mesh Mash + Catmull-Clark
// ═══════════════════════════════════════════════════════════════════════════════

// ── Icosahedron base mesh ───────────────────────────────────────────────────
struct ProceduralMesh {
  std::vector<Vector3> verts;
  std::vector<unsigned short> indices; // triangles
};

static ProceduralMesh MakeIcosahedron(float radius) {
  ProceduralMesh m;
  float t = (1.0f + sqrtf(5.0f)) / 2.0f;
  Vector3 base[] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
                    {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
                    {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
  for (auto &v : base) {
    float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    v.x *= radius / l;
    v.y *= radius / l;
    v.z *= radius / l;
  }
  m.verts.assign(base, base + 12);
  unsigned short idx[] = {0, 11, 5, 0, 5,  1,  0,  1,  7,  0,  7, 10, 0, 10, 11,
                          1, 5,  9, 5, 11, 4,  11, 10, 2,  10, 7, 6,  7, 1,  8,
                          3, 9,  4, 3, 4,  2,  3,  2,  6,  3,  6, 8,  3, 8,  9,
                          4, 9,  5, 2, 4,  11, 6,  2,  10, 8,  6, 7,  9, 8,  1};
  m.indices.assign(idx, idx + 60);
  return m;
}

// ── Mesh Mash: fuse extra displaced sub-icosahedra ─────────────────────────
static ProceduralMesh MeshMash(ProceduralMesh base, int extraCount,
                               float spread, float subScale) {
  ProceduralMesh result = base;
  for (int e = 0; e < extraCount; e++) {
    ProceduralMesh sub = MakeIcosahedron(subScale);
    Vector3 off = {(float)GetRandomValue(-100, 100) * 0.01f * spread,
                   (float)GetRandomValue(-30, 60) * 0.01f * spread,
                   (float)GetRandomValue(-100, 100) * 0.01f * spread};
    unsigned short baseIdx = (unsigned short)result.verts.size();
    for (auto &v : sub.verts) {
      result.verts.push_back({v.x + off.x, v.y + off.y, v.z + off.z});
    }
    for (auto i : sub.indices)
      result.indices.push_back(baseIdx + i);
  }
  return result;
}

// ── Average-vertex smoothing pass (simplified Catmull-Clark for triangles) ──
static void SmoothMesh(ProceduralMesh &m, int passes) {
  for (int pass = 0; pass < passes; pass++) {
    std::vector<Vector3> smoothed(m.verts.size(), {0, 0, 0});
    std::vector<int> counts(m.verts.size(), 0);
    for (size_t i = 0; i < m.indices.size(); i += 3) {
      unsigned short a = m.indices[i], b = m.indices[i + 1],
                     c = m.indices[i + 2];
      Vector3 center = {(m.verts[a].x + m.verts[b].x + m.verts[c].x) / 3.0f,
                        (m.verts[a].y + m.verts[b].y + m.verts[c].y) / 3.0f,
                        (m.verts[a].z + m.verts[b].z + m.verts[c].z) / 3.0f};
      for (int k = 0; k < 3; k++) {
        unsigned short idx = m.indices[i + k];
        smoothed[idx].x += center.x;
        smoothed[idx].y += center.y;
        smoothed[idx].z += center.z;
        counts[idx]++;
      }
    }
    for (size_t i = 0; i < m.verts.size(); i++) {
      if (counts[i] > 0) {
        float w = 0.5f; // blend 50% original + 50% smoothed
        float sx = smoothed[i].x / (float)counts[i];
        float sy = smoothed[i].y / (float)counts[i];
        float sz = smoothed[i].z / (float)counts[i];
        m.verts[i].x = m.verts[i].x * (1.0f - w) + sx * w;
        m.verts[i].y = m.verts[i].y * (1.0f - w) + sy * w;
        m.verts[i].z = m.verts[i].z * (1.0f - w) + sz * w;
      }
    }
  }
}

// ── Build Raylib Model from ProceduralMesh ──────────────────────────────────
static Model BuildRockModel(const ProceduralMesh &pm, Color col) {
  int triCount = (int)pm.indices.size() / 3;
  Mesh mesh = {0};
  mesh.vertexCount = (int)pm.indices.size();
  mesh.triangleCount = triCount;
  mesh.vertices = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
  mesh.normals = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
  mesh.colors = (unsigned char *)MemAlloc(mesh.vertexCount * 4);
  for (int i = 0; i < (int)pm.indices.size(); i += 3) {
    Vector3 a = pm.verts[pm.indices[i]];
    Vector3 b = pm.verts[pm.indices[i + 1]];
    Vector3 c = pm.verts[pm.indices[i + 2]];
    // face normal
    Vector3 ab = {b.x - a.x, b.y - a.y, b.z - a.z};
    Vector3 ac = {c.x - a.x, c.y - a.y, c.z - a.z};
    Vector3 n = {ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z,
                 ab.x * ac.y - ab.y * ac.x};
    float nl = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    if (nl > 0.0001f) {
      n.x /= nl;
      n.y /= nl;
      n.z /= nl;
    }
    Vector3 verts[3] = {a, b, c};
    for (int j = 0; j < 3; j++) {
      int vi = i + j;
      mesh.vertices[vi * 3] = verts[j].x;
      mesh.vertices[vi * 3 + 1] = verts[j].y;
      mesh.vertices[vi * 3 + 2] = verts[j].z;
      mesh.normals[vi * 3] = n.x;
      mesh.normals[vi * 3 + 1] = n.y;
      mesh.normals[vi * 3 + 2] = n.z;
      // Color variation per vertex for natural look
      int rv = GetRandomValue(-12, 12);
      mesh.colors[vi * 4 + 0] = (unsigned char)Clamp(col.r + rv, 0, 255);
      mesh.colors[vi * 4 + 1] = (unsigned char)Clamp(col.g + rv, 0, 255);
      mesh.colors[vi * 4 + 2] = (unsigned char)Clamp(col.b + rv - 5, 0, 255);
      mesh.colors[vi * 4 + 3] = 255;
    }
  }
  UploadMesh(&mesh, false);
  Model model = LoadModelFromMesh(mesh);
  return model;
}

// ── Generate one unique rock ────────────────────────────────────────────────
static Model GenerateProceduralRock(float baseRadius, Color col) {
  ProceduralMesh pm = MakeIcosahedron(baseRadius);
  int extras = GetRandomValue(3, 7);
  pm = MeshMash(pm, extras, baseRadius * 0.6f, baseRadius * 0.5f);
  SmoothMesh(pm, 2); // 2-pass Catmull-Clark averaging
  // Ground the mesh: offset so bottom sits at y=0
  float minY = 999.0f;
  for (auto &v : pm.verts)
    if (v.y < minY)
      minY = v.y;
  for (auto &v : pm.verts)
    v.y -= minY;
  return BuildRockModel(pm, col);
}

// ── Rock instance storage ───────────────────────────────────────────────────
static constexpr int MAX_ROCKS = 12;
struct RockInstance {
  float x, z, rotY, scale;
  Model model;
};
static RockInstance g_rockInstances[MAX_ROCKS];
static int g_numRocks = 0;

static void InitProceduralRocks() {
  g_numRocks = 0;
  Color rockCols[] = {{165, 140, 110, 255},
                      {180, 155, 120, 255},
                      {145, 125, 95, 255},
                      {190, 165, 130, 255}};
  for (int i = 0; i < MAX_ROCKS && g_numRocks < MAX_ROCKS; i++) {
    int rx = GetRandomValue(2, MAP_W - 3);
    int ry = GetRandomValue(2, MAP_H - 3);
    if (g_collision[ry][rx])
      continue; // skip occupied
    g_collision[ry][rx] = 1;
    RockInstance &ri = g_rockInstances[g_numRocks++];
    ri.x = (float)rx;
    ri.z = (float)ry;
    ri.rotY = (float)GetRandomValue(0, 360);
    ri.scale = 0.3f + (float)GetRandomValue(0, 40) * 0.01f;
    ri.model = GenerateProceduralRock(1.0f, rockCols[i % 4]);
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// §2  DESERT TENTS — AABB 4×4×4 Cube Meshes with 1×1 Door Void
// ═══════════════════════════════════════════════════════════════════════════════

struct TentInstance {
  int gx, gy;           // grid origin (top-left corner)
  float worldX, worldZ; // center world position
  Color wallCol, roofCol;
  const char *signText;
  int doorGX, doorGY; // door tile in grid coords
  Scene interiorScene;
  BoundingBox wallAABB;     // full tent collision
  BoundingBox doorAABB;     // 1×1 door trigger
  BoundingBox interiorAABB; // ceiling-hide trigger volume
  bool roofVisible;         // false = player inside, roof hidden
  bool frontWallVisible;    // false = player inside, south wall hidden too
  float roofAlpha;          // 0.0 = fully hidden, 1.0 = fully visible (X-Ray fade)
  float frontAlpha;         // same for front wall
};

static constexpr int MAX_TENTS = 4;
static TentInstance g_tentInstances[MAX_TENTS];
static int g_numTents = 0;

static void PlaceTent(int gx, int gy, Color wall, Color roof, const char *sign,
                      Scene interior) {
  if (g_numTents >= MAX_TENTS)
    return;
  TentInstance &t = g_tentInstances[g_numTents++];
  t.gx = gx;
  t.gy = gy;
  t.wallCol = wall;
  t.roofCol = roof;
  t.signText = sign;
  t.interiorScene = interior;
  // Center of 4×4 tent
  t.worldX = (float)gx + 2.0f;
  t.worldZ = (float)gy + 2.0f;
  // Door: centered on south face (bottom center, one tile outside)
  t.doorGX = gx + 2;
  t.doorGY = gy + TENT_SIZE; // centered south, just outside wall
  // Mark wall collision around the 4×4 footprint perimeter only.
  // Interior tiles are walkable (player walks in, ceiling hides).
  // Border ring (1 tile outside walls)
  for (int dy = -1; dy <= TENT_SIZE; dy++)
    for (int ddx = -1; ddx <= TENT_SIZE; ddx++) {
      int tx = gx + ddx, ty = gy + dy;
      // Only mark the outer ring and the wall tiles (not pure interior)
      bool isWall = (ddx == -1 || ddx == TENT_SIZE || dy == -1 || dy == TENT_SIZE);
      bool isEdge = (ddx == 0 || ddx == TENT_SIZE - 1 || dy == 0 || dy == TENT_SIZE - 1);
      if ((isWall || isEdge) && tx >= 0 && tx < MAP_W && ty >= 0 && ty < MAP_H)
        g_collision[ty][tx] = 1;
    }
  // Clear interior tiles (full walkable area inside the tent walls)
  for (int dy = 1; dy < TENT_SIZE - 1; dy++)
    for (int ddx = 1; ddx < TENT_SIZE - 1; ddx++) {
      int tx = gx + ddx, ty = gy + dy;
      if (tx >= 0 && tx < MAP_W && ty >= 0 && ty < MAP_H)
        g_collision[ty][tx] = 0;
    }
  // Clear south wall doorway corridor (≥1.2 units wide: 2 tiles centered)
  // The door is centered at column gx+2. Clear columns gx+1 and gx+2 on the
  // south edge row (dy == TENT_SIZE-1) so the player can walk through.
  for (int dc = -1; dc <= 0; dc++) {
    int doorCol = t.doorGX + dc; // gx+1 and gx+2
    int southEdge = gy + TENT_SIZE - 1;
    if (doorCol >= 0 && doorCol < MAP_W && southEdge >= 0 && southEdge < MAP_H)
      g_collision[southEdge][doorCol] = 0;
  }
  // Clear the outer door tile and the approach tile south of it
  for (int extra = 0; extra <= 1; extra++) {
    int ty = t.doorGY + extra;
    for (int dc = -1; dc <= 0; dc++) {
      int tx = t.doorGX + dc;
      if (tx >= 0 && tx < MAP_W && ty >= 0 && ty < MAP_H)
        g_collision[ty][tx] = 0;
    }
  }
  // AABB: full tent walls (4×4×4 world units)
  t.wallAABB.min = {t.worldX - 2.0f, 0.0f, t.worldZ - 2.0f};
  t.wallAABB.max = {t.worldX + 2.0f, 4.0f, t.worldZ + 2.0f};
  // AABB: door trigger (0.4 wide × 0.5 deep, centered on door tile)
  float ddx = (float)t.doorGX + 0.5f, ddz = (float)t.doorGY + 0.5f;
  t.doorAABB.min = {ddx - 0.2f, 0.0f, ddz - 0.25f};
  t.doorAABB.max = {ddx + 0.2f, 2.0f, ddz + 0.25f};
  // AABB: interior volume for ceiling-hide (covers interior + doorway approach)
  t.interiorAABB.min = {t.worldX - 1.6f, 0.0f, t.worldZ - 1.6f};
  t.interiorAABB.max = {t.worldX + 1.6f, 4.0f, t.worldZ + 2.4f}; // extends south through doorway
  t.roofVisible = true;
  t.frontWallVisible = true;
  t.roofAlpha = 1.0f;
  t.frontAlpha = 1.0f;
}

// ═══════════════════════════════════════════════════════════════════════════════
// UpdateTentVisibility — "X-Ray" cutaway with smooth alpha fade
//   Tests player AABB vs tent interiorAABB.
//   Interpolates roofAlpha / frontAlpha over ROOF_FADE_TIME (0.4s).
//   roofVisible/frontWallVisible are set based on alpha threshold.
// ═══════════════════════════════════════════════════════════════════════════════
static constexpr float ROOF_FADE_TIME = 0.4f; // seconds for X-Ray transition

static void UpdateTentVisibility() {
  float dt = GetFrameTime();
  float fadeSpeed = dt / ROOF_FADE_TIME; // ~2.5 per second at 60fps
  BoundingBox playerBox = {
      {g_player.posX - 0.35f, 0.0f, g_player.posZ - 0.35f},
      {g_player.posX + 0.35f, 2.0f, g_player.posZ + 0.35f}};
  for (int i = 0; i < g_numTents; i++) {
    TentInstance &t = g_tentInstances[i];
    bool inside = CheckCollisionBoxes(playerBox, t.interiorAABB);
    float targetAlpha = inside ? 0.0f : 1.0f;
    // Smooth interpolation toward target
    if (t.roofAlpha < targetAlpha)
      t.roofAlpha = fminf(t.roofAlpha + fadeSpeed, targetAlpha);
    else if (t.roofAlpha > targetAlpha)
      t.roofAlpha = fmaxf(t.roofAlpha - fadeSpeed, targetAlpha);
    if (t.frontAlpha < targetAlpha)
      t.frontAlpha = fminf(t.frontAlpha + fadeSpeed, targetAlpha);
    else if (t.frontAlpha > targetAlpha)
      t.frontAlpha = fmaxf(t.frontAlpha - fadeSpeed, targetAlpha);
    // Visibility flags (used for drawing cutaway interior vs roof)
    t.roofVisible = (t.roofAlpha > 0.01f);
    t.frontWallVisible = (t.frontAlpha > 0.01f);
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// DrawTent — Solid closed-box tent (truncated parallelepiped)
//
//   Geometry: 4 solid walls + roof as closed geometry.
//   Front (south) wall has a recessed rectangular doorway (physical hole).
//   Four wooden corner poles, canvas texture strips on walls.
//   Cutaway: roof + front wall hidden when player is inside.
//   Interior furniture drawn in-place when cutaway is active.
// ═══════════════════════════════════════════════════════════════════════════════
static void DrawTentInteriorFurniture(const TentInstance &t, int tentIdx);

// Apply alpha factor to a color (for X-Ray fade)
static Color FadeColor(Color c, float alpha) {
  return {c.r, c.g, c.b, (unsigned char)(c.a * Clamp(alpha, 0.0f, 1.0f))};
}

static void DrawTent(const TentInstance &t) {
  float cx = t.worldX, cz = t.worldZ;
  float hs = 2.0f;       // half-size on XZ
  float wh = 3.5f;       // wall height
  float wt = 0.18f;      // wall thickness
  float taperBot = 0.25f; // extra width at base (truncated parallelepiped)
  float taperTop = -0.20f;// narrower at top
  Color wc = t.wallCol;
  // Two-tone: sunlit (north/top) vs desert-worn (sides/south)
  Color wcSun  = {(unsigned char)Clamp(wc.r * 1.15f, 0, 255),
                  (unsigned char)Clamp(wc.g * 1.10f, 0, 255),
                  (unsigned char)Clamp(wc.b * 1.00f, 0, 255), 255};
  Color wcDark = {(unsigned char)(wc.r * 0.65f),
                  (unsigned char)(wc.g * 0.60f),
                  (unsigned char)(wc.b * 0.55f), 255};
  Color wcMid  = {(unsigned char)(wc.r * 0.82f),
                  (unsigned char)(wc.g * 0.78f),
                  (unsigned char)(wc.b * 0.72f), 255};

  // ── Solid walls as 3 stacked layers (truncated parallelepiped taper) ───
  int layers = 3;
  for (int l = 0; l < layers; l++) {
    float frac = ((float)l + 0.5f) / (float)layers;
    float layerH = wh / (float)layers;
    float y = frac * wh;
    float taper = Lerp(taperBot, taperTop, frac);
    float hsL = hs + taper; // half-size at this layer

    // North wall (back — sunlit, always visible)
    DrawCube({cx, y, cz - hsL}, hsL * 2.0f, layerH, wt, wcSun);

    // South wall (front) — split around doorway, fades with frontAlpha
    if (t.frontWallVisible) {
      Color fwCol = FadeColor(wcDark, t.frontAlpha);
      float doorHalfW = 0.55f; // half-width of door opening
      float doorH = 2.2f;      // door height
      float leftW = hsL - doorHalfW;
      float rightW = hsL - doorHalfW;
      // Left panel
      DrawCube({cx - doorHalfW - leftW * 0.5f, y, cz + hsL},
               leftW, layerH, wt, fwCol);
      // Right panel
      DrawCube({cx + doorHalfW + rightW * 0.5f, y, cz + hsL},
               rightW, layerH, wt, fwCol);
      // Lintel (above door)
      if (y - layerH * 0.5f < doorH && y + layerH * 0.5f > doorH) {
        float aboveY = (doorH + y + layerH * 0.5f) * 0.5f;
        float aboveH = (y + layerH * 0.5f) - doorH;
        if (aboveH > 0.01f)
          DrawCube({cx, aboveY, cz + hsL}, doorHalfW * 2.0f, aboveH, wt, fwCol);
      } else if (y - layerH * 0.5f >= doorH) {
        DrawCube({cx, y, cz + hsL}, doorHalfW * 2.0f, layerH, wt, fwCol);
      }
    }

    // East wall (always visible)
    DrawCube({cx + hsL, y, cz}, wt, layerH, hsL * 2.0f, wcMid);
    // West wall (always visible)
    DrawCube({cx - hsL, y, cz}, wt, layerH, hsL * 2.0f, wc);
  }

  // ── Canvas texture strips (horizontal bands across walls) ──────────────
  for (int s = 0; s < 4; s++) {
    float stripY = 0.6f + s * 0.85f;
    if (stripY > wh) break;
    float frac2 = stripY / wh;
    float hsS = hs + Lerp(taperBot, taperTop, frac2);
    Color stripCol = {(unsigned char)(wc.r * 0.72f),
                      (unsigned char)(wc.g * 0.68f),
                      (unsigned char)(wc.b * 0.62f), 255};
    // Bands on north and side walls
    DrawCube({cx, stripY, cz - hsS + 0.01f}, hsS * 1.9f, 0.06f, 0.02f, stripCol);
    DrawCube({cx + hsS - 0.01f, stripY, cz}, 0.02f, 0.06f, hsS * 1.9f, stripCol);
    DrawCube({cx - hsS + 0.01f, stripY, cz}, 0.02f, 0.06f, hsS * 1.9f, stripCol);
  }

  // ── Recessed doorway (fades with front wall) ──────────────────────────
  if (t.frontWallVisible) {
    float fa = t.frontAlpha;
    float doorHW = 0.55f;
    float doorH = 2.2f;
    float recessDepth = 0.35f;
    // Dark void inside the doorway
    DrawCube({cx, doorH * 0.5f, cz + hs - recessDepth * 0.5f},
             doorHW * 2.0f, doorH, recessDepth, FadeColor({10, 8, 6, 255}, fa));
    // Door frame (dark wooden border)
    Color frameCol = FadeColor({65, 48, 32, 255}, fa);
    DrawCube({cx - doorHW - 0.06f, doorH * 0.5f, cz + hs + wt * 0.5f},
             0.12f, doorH, wt + 0.04f, frameCol);
    DrawCube({cx + doorHW + 0.06f, doorH * 0.5f, cz + hs + wt * 0.5f},
             0.12f, doorH, wt + 0.04f, frameCol);
    DrawCube({cx, doorH + 0.06f, cz + hs + wt * 0.5f},
             doorHW * 2.0f + 0.24f, 0.12f, wt + 0.04f, frameCol);
  }

  // Threshold stone (always visible — on the ground)
  DrawCube({cx, 0.02f, cz + hs + 0.10f}, 1.2f, 0.04f, 0.40f, {55, 42, 30, 255});

  // ── 4 wooden support poles at corners ──────────────────────────────────
  Color poleCol = {94, 70, 46, 255};
  float pOff = hs + taperBot - 0.05f;
  DrawCylinder({cx - pOff, 0.0f, cz - pOff}, 0.08f, 0.06f, wh + 0.8f, 8, poleCol);
  DrawCylinder({cx + pOff, 0.0f, cz - pOff}, 0.08f, 0.06f, wh + 0.8f, 8, poleCol);
  DrawCylinder({cx - pOff, 0.0f, cz + pOff}, 0.08f, 0.06f, wh + 0.8f, 8, poleCol);
  DrawCylinder({cx + pOff, 0.0f, cz + pOff}, 0.08f, 0.06f, wh + 0.8f, 8, poleCol);

  // ── Interior floor + furniture (drawn when roof not fully opaque) ────────
  if (t.roofAlpha < 0.99f) {
    float gy = GetDuneHeight(cx, cz) + 0.03f;
    // Rug / floor mat
    DrawCube({cx, gy, cz}, hs * 1.7f, 0.02f, hs * 1.7f, {85, 65, 42, 255});
    DrawCube({cx, gy + 0.005f, cz}, hs * 1.4f, 0.015f, hs * 1.4f, {95, 72, 48, 255});
    // Interior furniture based on tent index
    int idx = -1;
    for (int i = 0; i < g_numTents; i++) {
      if (&g_tentInstances[i] == &t) { idx = i; break; }
    }
    if (idx >= 0) DrawTentInteriorFurniture(t, idx);
    // Hanging lantern
    DrawSphere({cx, wh - 0.4f, cz}, 0.10f, {255, 205, 120, 255});
    DrawSphere({cx, wh - 0.4f, cz}, 0.25f, {255, 190, 100, 80});
  }

  // ── Roof / ceiling (X-Ray fade with roofAlpha) ─────────────────────────
  if (t.roofVisible) {
    float ra = t.roofAlpha;
    Color rc = t.roofCol;
    Color rcBright = FadeColor({(unsigned char)Clamp(rc.r * 1.20f, 0, 255),
                      (unsigned char)Clamp(rc.g * 1.15f, 0, 255),
                      (unsigned char)Clamp(rc.b * 1.05f, 0, 255), 255}, ra);
    Color rcDark   = FadeColor({(unsigned char)(rc.r * 0.70f),
                      (unsigned char)(rc.g * 0.65f),
                      (unsigned char)(rc.b * 0.60f), 255}, ra);
    Color rcMid    = FadeColor(rc, ra);
    // Flat roof slab (slightly overhanging)
    float roofOverhang = 0.25f;
    float roofHS = hs + taperTop + roofOverhang;
    DrawCube({cx, wh + 0.15f, cz}, roofHS * 2.0f, 0.30f, roofHS * 2.0f, rcBright);
    // Peaked ridge on top
    DrawCube({cx, wh + 0.45f, cz}, roofHS * 1.4f, 0.25f, roofHS * 1.4f, rcMid);
    DrawCube({cx, wh + 0.70f, cz}, roofHS * 0.7f, 0.20f, roofHS * 0.7f, rcDark);
    // Ridge beam
    DrawCube({cx, wh + 0.85f, cz}, roofHS * 0.3f, 0.08f, roofHS * 1.8f, rcDark);
    // Canvas strips across roof
    for (int f = 0; f < 5; f++) {
      float fx = -1.5f + f * 0.75f;
      float lift = sinf(fx * 1.55f) * 0.05f;
      DrawCube({cx + fx, wh + 0.35f + lift, cz}, 0.30f, 0.04f, roofHS * 1.9f,
               FadeColor({(unsigned char)(rc.r * 0.74f), (unsigned char)(rc.g * 0.74f),
                (unsigned char)(rc.b * 0.74f), 255}, ra));
    }
    // Draped eave trim (north + south)
    for (int d = 0; d < 5; d++) {
      float dx2 = -1.6f + d * 0.8f;
      float sagF = sinf(dx2 * 1.6f) * 0.15f;
      float sagB = cosf(dx2 * 1.4f) * 0.12f;
      DrawCube({cx + dx2, wh + 0.02f + sagF, cz + roofHS + 0.04f},
               0.75f, 0.12f, 0.05f, rcDark);
      DrawCube({cx + dx2, wh + 0.02f + sagB, cz - roofHS - 0.04f},
               0.75f, 0.12f, 0.05f, rcDark);
    }
  }

  // ── Sign on RIGHT side of entrance ─────────────────────────────────────
  if (t.signText) {
    float signX = cx + 1.6f, signZ = cz + hs + 0.5f;
    DrawCube({signX, 0.75f, signZ}, 0.07f, 1.5f, 0.07f, {80, 60, 40, 255});
    DrawCube({signX, 1.55f, signZ}, 1.1f, 0.45f, 0.05f, {160, 130, 80, 255});
  }

  // ── Guy ropes from tent top to ground stakes ───────────────────────────
  Color ropeCol = {140, 120, 90, 255};
  float rOff = hs + taperBot - 0.05f;
  Vector3 ropeTop[4] = {{cx - rOff, wh + 0.1f, cz - rOff},
                        {cx + rOff, wh + 0.1f, cz - rOff},
                        {cx - rOff, wh + 0.1f, cz + rOff},
                        {cx + rOff, wh + 0.1f, cz + rOff}};
  Vector3 ropeGnd[4] = {{cx - hs - 1.2f, 0.0f, cz - hs - 1.0f},
                        {cx + hs + 1.2f, 0.0f, cz - hs - 1.0f},
                        {cx - hs - 1.2f, 0.0f, cz + hs + 1.0f},
                        {cx + hs + 1.2f, 0.0f, cz + hs + 1.0f}};
  for (int r = 0; r < 4; r++) {
    DrawLine3D(ropeTop[r], ropeGnd[r], ropeCol);
    DrawCylinder(ropeGnd[r], 0.04f, 0.04f, 0.18f, 8, {90, 70, 50, 255});
  }
}

// ── Interior furniture drawn inside tent when cutaway is active ──────────────
static void DrawTentInteriorFurniture(const TentInstance &t, int tentIdx) {
  float cx = t.worldX, cz = t.worldZ;
  Color woodCol = {110, 82, 54, 255};
  Color woodDk  = {82, 60, 38, 255};
  Color fabricCol = {124, 92, 58, 255};
  switch (tentIdx) {
  case 0: // Fatima's Tent — table + shelf
    DrawCube({cx - 1.0f, 0.40f, cz - 1.0f}, 1.8f, 0.08f, 1.0f, woodCol);
    DrawCube({cx - 1.6f, 0.20f, cz - 1.0f}, 0.08f, 0.40f, 0.08f, woodDk);
    DrawCube({cx - 0.4f, 0.20f, cz - 1.0f}, 0.08f, 0.40f, 0.08f, woodDk);
    DrawCube({cx + 1.2f, 1.0f, cz - 1.6f}, 0.90f, 2.0f, 0.25f, woodDk);
    break;
  case 1: // Scholar's Study — desk + bookshelf
    DrawCube({cx, 0.40f, cz - 1.1f}, 2.0f, 0.08f, 0.8f, woodCol);
    DrawCube({cx - 0.8f, 0.20f, cz - 1.1f}, 0.08f, 0.40f, 0.08f, woodDk);
    DrawCube({cx + 0.8f, 0.20f, cz - 1.1f}, 0.08f, 0.40f, 0.08f, woodDk);
    DrawCube({cx - 1.5f, 1.0f, cz - 1.5f}, 0.30f, 2.0f, 0.80f, woodDk);
    break;
  case 2: // Yara's Bazaar — long counter + crates
    DrawCube({cx, 0.45f, cz - 0.5f}, 3.0f, 0.10f, 0.9f, fabricCol);
    DrawCube({cx - 1.2f, 0.22f, cz - 0.5f}, 0.08f, 0.44f, 0.08f, woodDk);
    DrawCube({cx + 1.2f, 0.22f, cz - 0.5f}, 0.08f, 0.44f, 0.08f, woodDk);
    DrawCube({cx - 1.1f, 0.30f, cz - 1.5f}, 0.70f, 0.60f, 0.70f, woodCol);
    DrawCube({cx + 1.1f, 0.30f, cz - 1.5f}, 0.70f, 0.60f, 0.70f, woodCol);
    break;
  case 3: // Kai's Workshop — workbench + toolrack
    DrawCube({cx - 0.8f, 0.42f, cz - 0.9f}, 2.0f, 0.08f, 1.0f, woodCol);
    DrawCube({cx - 1.6f, 0.21f, cz - 0.9f}, 0.08f, 0.42f, 0.08f, woodDk);
    DrawCube({cx + 0.0f, 0.21f, cz - 0.9f}, 0.08f, 0.42f, 0.08f, woodDk);
    DrawCube({cx + 1.2f, 1.0f, cz - 1.5f}, 0.60f, 2.0f, 0.20f, woodDk);
    DrawCube({cx + 1.2f, 0.30f, cz - 1.5f}, 1.0f, 0.60f, 0.60f, fabricCol);
    break;
  }
}


// ═══════════════════════════════════════════════════════════════════════════════
// §3  WIND LINES — Minimalist Screen-Space Gusts
// ═══════════════════════════════════════════════════════════════════════════════
static constexpr int MICRO_MAX = 96;
struct MicroParticle {
  float x, y;
  float phase;
  float speed;
  float alpha;
  int size;
};
static MicroParticle g_microParticles[MICRO_MAX];
static bool g_microInit = false;

static constexpr int WIND_MAX_LINES = 18;
struct WindLine {
  float x, y;
  float length;
  float thickness;
  float speed;
  float alpha;
};
static WindLine g_windLines[WIND_MAX_LINES];
static int g_windCount = 0;
static float g_gustTimer = 0.0f;
static float g_nextGust = 1.1f;

static void InitMicroParticles() {
  if (g_microInit)
    return;
  g_microInit = true;
  for (int i = 0; i < MICRO_MAX; i++) {
    g_microParticles[i].x = (float)GetRandomValue(0, SCREEN_W);
    g_microParticles[i].y = (float)GetRandomValue(0, SCREEN_H);
    g_microParticles[i].phase =
        (float)GetRandomValue(0, 628) * 0.01f;
    g_microParticles[i].speed =
        14.0f + (float)GetRandomValue(0, 20) * 0.5f;
    g_microParticles[i].alpha =
        0.08f + (float)GetRandomValue(0, 4) * 0.01f; // ~0.10 per spec
    g_microParticles[i].size = GetRandomValue(1, 2);
  }
}

static void UpdateMicroParticles(float dt) {
  InitMicroParticles();
  for (int i = 0; i < MICRO_MAX; i++) {
    MicroParticle &p = g_microParticles[i];
    p.x += p.speed * dt;
    if (p.x > SCREEN_W + 2.0f) {
      p.x = (float)GetRandomValue(-40, -2);
      p.y = (float)GetRandomValue(0, SCREEN_H);
      p.phase = (float)GetRandomValue(0, 628) * 0.01f;
      p.speed = 14.0f + (float)GetRandomValue(0, 20) * 0.5f;
      p.alpha = 0.12f + (float)GetRandomValue(0, 8) * 0.01f;
      p.size = GetRandomValue(1, 2);
    }
  }
}

static void SpawnWindGust() {
  int n = GetRandomValue(3, 5);
  float baseSpeed = (float)GetRandomValue(400, 600);
  for (int i = 0; i < n && g_windCount < WIND_MAX_LINES; i++) {
    WindLine wl;
    wl.length = (float)GetRandomValue(20, 60);
    wl.thickness = (float)GetRandomValue(1, 2);
    wl.y = (float)GetRandomValue(8, SCREEN_H - 8);
    wl.x = -(wl.length + (float)GetRandomValue(0, 50));
    wl.speed = baseSpeed * (0.9f + (float)GetRandomValue(0, 20) * 0.005f) -
               FIXED_WIND_SPEED;
    wl.alpha = (GetRandomValue(0, 1) == 0) ? 0.10f : 0.15f;
    g_windLines[g_windCount++] = wl;
  }
}

static void UpdateVFX(float dt) {
  UpdateMicroParticles(dt);
  g_gustTimer += dt;
  if (g_gustTimer >= g_nextGust) {
    g_gustTimer = 0.0f;
    g_nextGust = 8.5f + (float)GetRandomValue(0, 400) * 0.01f;
    SpawnWindGust();
  }
  for (int i = 0; i < g_windCount; i++) {
    WindLine &wl = g_windLines[i];
    wl.x += (FIXED_WIND_SPEED * GetFrameTime()) + wl.speed * GetFrameTime();
    if (wl.x > SCREEN_W + wl.length + 10.0f) {
      wl.x = -(wl.length + (float)GetRandomValue(0, 50));
      wl.y = (float)GetRandomValue(8, SCREEN_H - 8);
    }
  }
}

static void DrawVFX() {
  for (int i = 0; i < MICRO_MAX; i++) {
    const MicroParticle &p = g_microParticles[i];
    float yo = sinf(g_time * 2.1f + p.phase) * 2.0f;
    Color c = ColorAlpha({247, 238, 222, 255}, p.alpha);
    DrawRectangle((int)p.x, (int)(p.y + yo), p.size, p.size, c);
  }
  for (int i = 0; i < g_windCount; i++) {
    const WindLine &wl = g_windLines[i];
    Color tail = ColorAlpha(WHITE, 0.0f);
    Color front = ColorAlpha(WHITE, wl.alpha);
    int rx = (int)wl.x;
    int ry = (int)(wl.y - wl.thickness * 0.5f);
    int rw = (int)wl.length;
    int rh = (int)wl.thickness;
    DrawRectangleGradientH(rx, ry, rw, rh, tail, front);
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// §4  CYLINDRICAL BILLBOARD — NPCs always face camera on Y axis
// ═══════════════════════════════════════════════════════════════════════════════

// Draw a cylindrical billboard (locked Y-up, rotates around Y to face camera)
static void DrawCylindricalBillboard(Camera3D cam, Texture2D tex, Vector3 pos,
                                     float width, float height, Color tint) {
  // Use raylib's DrawBillboardRec for proper shader/batch integration
  // pos is bottom-center; DrawBillboardRec draws centered, so shift up by half
  // height
  Vector3 center = {pos.x, pos.y + height * 0.5f, pos.z};
  Rectangle src = {0, 0, (float)tex.width, (float)tex.height};
  Vector2 size = {width, height};
  DrawBillboardRec(cam, tex, src, center, size, tint);
}

// ═══════════════════════════════════════════════════════════════════════════════
// §5  PCF SOFT SHADOWS — Percentage Closer Filtering for sun shadows
// ═══════════════════════════════════════════════════════════════════════════════

// Forward decl for dune height sampling used by shadow placement
static float GetDuneHeight(float x, float z);

// Sun direction (constant, no flicker — directional light from top-right)
static const Vector3 SUN_DIR = {-0.5f, -0.85f, 0.35f}; // normalized approx

// Project shadow as a dark stretched ellipse on ground plane
static void DrawSoftShadow(Vector3 objPos, float radius, float height) {
  // Shadow offset from sun direction projected onto ground
  float shadowX = objPos.x - SUN_DIR.x * height * 0.6f;
  float shadowZ = objPos.z - SUN_DIR.z * height * 0.6f;
  // PCF approximation: draw multiple overlapping transparent ellipses
  float baseAlpha = 18.0f;
  int pcfSamples = 5;
  float pcfSpread = radius * 0.25f;
  for (int i = 0; i < pcfSamples; i++) {
    float angle = (float)i * (6.2832f / (float)pcfSamples);
    float ox = cosf(angle) * pcfSpread;
    float oz = sinf(angle) * pcfSpread;
    // Stretched ellipse: wider in sun direction
    float gy = GetDuneHeight(shadowX + ox, shadowZ + oz) + 0.02f;
    Vector3 p = {shadowX + ox, gy, shadowZ + oz};
    DrawCylinder(p, radius * 1.2f, radius * 1.2f, 0.01f, 12,
                 {0, 0, 0, (unsigned char)Clamp(baseAlpha, 0, 60)});
  }
  // Center shadow (stronger)
  float gyc = GetDuneHeight(shadowX, shadowZ) + 0.015f;
  DrawCylinder({shadowX, gyc, shadowZ}, radius, radius, 0.01f, 12,
               {0, 0, 0, (unsigned char)Clamp(baseAlpha * 2.0f, 0, 50)});
}

// Draw tent shadow (large, soft PCF)
static void DrawTentShadow(const TentInstance &t) {
  float shadowX = t.worldX - SUN_DIR.x * 4.0f * 0.5f;
  float shadowZ = t.worldZ - SUN_DIR.z * 4.0f * 0.5f;
  int samples = 7;
  float spread = 0.8f;
  for (int i = 0; i < samples; i++) {
    float a = (float)i * (6.2832f / (float)samples);
    float sx = shadowX + cosf(a) * spread;
    float sz = shadowZ + sinf(a) * spread;
    float gy = GetDuneHeight(sx, sz) + 0.01f;
    DrawCube({sx, gy, sz},
             4.2f, 0.01f, 4.2f, {0, 0, 0, 12});
  }
  float gyc2 = GetDuneHeight(shadowX, shadowZ) + 0.005f;
  DrawCube({shadowX, gyc2, shadowZ}, 4.0f, 0.01f, 4.0f, {0, 0, 0, 25});
}

// ═══════════════════════════════════════════════════════════════════════════════
// §6  FULL POST-PROCESSING PIPELINE
// ═══════════════════════════════════════════════════════════════════════════════

static void ApplyFullPostProcess(float time) {
  // 1. Extract bright pixels for bloom
  BeginTextureMode(g_brightFBO);
  ClearBackground(BLACK);
  BeginShaderMode(g_shBloomExtract);
  float threshold = 0.75f;
  SetShaderValue(g_shBloomExtract, g_locThreshold, &threshold,
                 SHADER_UNIFORM_FLOAT);
  DrawFBOQuadScaled(g_sceneFBO, SCREEN_W / 2, SCREEN_H / 2);
  EndShaderMode();
  EndTextureMode();

  // 2. Gaussian blur — single pass (reduced 60% to keep pixel art crisp)
  for (int pass = 0; pass < 1; pass++) {
    BeginTextureMode(g_blurA);
    ClearBackground(BLACK);
    BeginShaderMode(g_shBlur);
    float dirH[2] = {1.0f / (float)(SCREEN_W / 2), 0};
    SetShaderValue(g_shBlur, g_locBlurDir, dirH, SHADER_UNIFORM_VEC2);
    DrawFBOQuad(pass == 0 ? g_brightFBO : g_blurB);
    EndShaderMode();
    EndTextureMode();

    BeginTextureMode(g_blurB);
    ClearBackground(BLACK);
    BeginShaderMode(g_shBlur);
    float dirV[2] = {0, 1.0f / (float)(SCREEN_H / 2)};
    SetShaderValue(g_shBlur, g_locBlurDir, dirV, SHADER_UNIFORM_VEC2);
    DrawFBOQuad(g_blurA);
    EndShaderMode();
    EndTextureMode();
  }

  // 3. God rays
  BeginTextureMode(g_godrayFBO);
  ClearBackground(BLACK);
  BeginShaderMode(g_shGodrays);
  float sunPos[2] = {0.8f, 0.15f}; // top-right sun position in UV
  SetShaderValue(g_shGodrays, g_locSunPos, sunPos, SHADER_UNIFORM_VEC2);
  DrawFBOQuadScaled(g_sceneFBO, SCREEN_W / 2, SCREEN_H / 2);
  EndShaderMode();
  EndTextureMode();

  // 4. Final composite: scene + bloom + godrays + heat haze + ACES + vignette
  // Render into g_pixelFBO so we can pixelate the result
  BeginTextureMode(g_pixelFBO);
  ClearBackground(BLACK);
  BeginShaderMode(g_shComposite);
  // Bind bloom and godray textures to slots 1 and 2
  SetShaderValueTexture(g_shComposite, g_locBloomTex, g_blurB.texture);
  SetShaderValueTexture(g_shComposite, g_locGodrayTex, g_godrayFBO.texture);
  float bloomStr = 0.20f, godrayStr = 0.08f; // controlled bloom — pixel art stays crisp
  SetShaderValue(g_shComposite, g_locBloomStr, &bloomStr, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_shComposite, g_locGodrayStr, &godrayStr,
                 SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_shComposite, g_locCompTime, &time, SHADER_UNIFORM_FLOAT);
  float exp = EXPOSURE;
  SetShaderValue(g_shComposite, g_locCompExposure, &exp, SHADER_UNIFORM_FLOAT);
  float res[2] = {(float)SCREEN_W, (float)SCREEN_H};
  SetShaderValue(g_shComposite, g_locCompRes, res, SHADER_UNIFORM_VEC2);
  DrawFBOQuad(g_sceneFBO);
  EndShaderMode();
  // Don't end texture mode here — caller manages g_pixelFBO
}

// ═══════════════════════════════════════════════════════════════════════════════
// §6b PERLIN NOISE DUNE TERRAIN — Heightmap mesh with soft sand swells
// ═══════════════════════════════════════════════════════════════════════════════

// ── Simple 2D Perlin noise (value noise with smoothstep) ────────────────────
static float HashNoise(int x, int y) {
  int n = x + y * 57;
  n = (n << 13) ^ n;
  return (1.0f - ((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) /
                     1073741824.0f);
}
static float SmoothNoise2D(float x, float y) {
  int ix = (int)floorf(x), iy = (int)floorf(y);
  float fx = x - ix, fy = y - iy;
  // Smoothstep
  fx = fx * fx * (3.0f - 2.0f * fx);
  fy = fy * fy * (3.0f - 2.0f * fy);
  float a = HashNoise(ix, iy), b = HashNoise(ix + 1, iy);
  float c = HashNoise(ix, iy + 1), d = HashNoise(ix + 1, iy + 1);
  return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
static float PerlinFBM(float x, float y, int octaves, float persistence) {
  float total = 0, amplitude = 1.0f, frequency = 1.0f, maxVal = 0;
  for (int i = 0; i < octaves; i++) {
    total += SmoothNoise2D(x * frequency, y * frequency) * amplitude;
    maxVal += amplitude;
    amplitude *= persistence;
    frequency *= 2.0f;
  }
  return total / maxVal;
}

// ── Dune heightmap: returns height at world (x,z) ──────────────────────────
static constexpr float DUNE_MAX_HEIGHT = 3.0f;
static constexpr float DUNE_FREQUENCY = 0.16f;

static float GetDuneHeight(float x, float z) {
  float n = PerlinFBM(x * DUNE_FREQUENCY, z * DUNE_FREQUENCY, 5, 0.5f);
  float h = n - 0.5f; // center around 0
  float s = powf(fabsf(h), 1.6f);
  h = 0.7f * h + 0.3f * (h >= 0.0f ? s : -s); // smooth rolling peaks
  return h * DUNE_MAX_HEIGHT;
}

// ── Generate terrain mesh (single mesh for entire MAP) ──────────────────────
static Model g_terrainModel;
static Texture2D g_sandTexture;
static Texture2D g_sandNormalTexture;
static Texture2D g_sandGritTexture;
static bool g_terrainReady = false;

static Texture2D GenerateSandTexture() {
  int sz = 128;
  Image img = GenImageColor(sz, sz, {210, 185, 140, 255});
  // Multi-pass noise for seamless sand grain look
  for (int y = 0; y < sz; y++)
    for (int x = 0; x < sz; x++) {
      float n = PerlinFBM((float)x * 0.15f, (float)y * 0.15f, 4, 0.6f);
      int v = (int)(n * 12.0f);
      Color c = GetImageColor(img, x, y);
      c.r = (unsigned char)Clamp(c.r + v, 150, 245);
      c.g = (unsigned char)Clamp(c.g + v - 2, 130, 220);
      c.b = (unsigned char)Clamp(c.b + v - 6, 90, 195);
      ImageDrawPixel(&img, x, y, c);
    }
  Texture2D t = LoadTextureFromImage(img);
  SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
  SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
  UnloadImage(img);
  return t;
}

static Texture2D GenerateSandNormalTexture() {
  int sz = 128;
  Image img = GenImageColor(sz, sz, {128, 128, 255, 255});
  for (int y = 0; y < sz; y++)
    for (int x = 0; x < sz; x++) {
      float u = (float)x / (float)sz;
      float v = (float)y / (float)sz;
      float rip = sinf(u * 32.0f) * 0.5f + 0.5f;
      rip += sinf((u + v * 0.5f) * 18.0f) * 0.5f + 0.5f;
      rip *= 0.5f; // subtle
      unsigned char r = (unsigned char)Clamp(128 + rip * 40.0f, 0, 255);
      unsigned char g = (unsigned char)Clamp(128 + rip * 40.0f, 0, 255);
      unsigned char b = 255;
      ImageDrawPixel(&img, x, y, {r, g, b, 255});
    }
  Texture2D t = LoadTextureFromImage(img);
  SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
  SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
  UnloadImage(img);
  return t;
}

static Texture2D GenerateSandGritTexture() {
  int sz = 128;
  Image img = GenImageColor(sz, sz, {200, 170, 120, 255});
  for (int y = 0; y < sz; y++)
    for (int x = 0; x < sz; x++) {
      float u = (float)x / (float)sz;
      float v = (float)y / (float)sz;
      float n = PerlinFBM(u * 8.0f, v * 8.0f, 3, 0.65f);
      bool high = n > 0.52f;
      Color c = high ? Color{180, 150, 100, 255} : Color{120, 90, 60, 255};
      ImageDrawPixel(&img, x, y, c);
    }
  Texture2D t = LoadTextureFromImage(img);
  SetTextureFilter(t, TEXTURE_FILTER_POINT);
  SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
  UnloadImage(img);
  return t;
}

static void GenerateTerrainMesh() {
  // Subdivided grid: 2 triangles per tile, with heightmap displacement
  int resX = MAP_W * 4, resZ = MAP_H * 4; // 4x subdivision for smoother dunes
  int vertCount = (resX + 1) * (resZ + 1);
  int triCount = resX * resZ * 2;

  Mesh mesh = {0};
  mesh.vertexCount = triCount * 3;
  mesh.triangleCount = triCount;
  mesh.vertices = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
  mesh.texcoords = (float *)MemAlloc(mesh.vertexCount * 2 * sizeof(float));
  mesh.normals = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
  mesh.colors = (unsigned char *)MemAlloc(mesh.vertexCount * 4);

  // Build height cache
  std::vector<float> heights((resX + 1) * (resZ + 1));
  float stepX = (float)MAP_W / (float)resX;
  float stepZ = (float)MAP_H / (float)resZ;
  float minH = 1e9f, maxH = -1e9f;
  for (int zy = 0; zy <= resZ; zy++)
    for (int zx = 0; zx <= resX; zx++) {
      float wx = zx * stepX - 0.5f;
      float wz = zy * stepZ - 0.5f;
      float h = GetDuneHeight(wx, wz);
      heights[zy * (resX + 1) + zx] = h;
      if (h < minH)
        minH = h;
      if (h > maxH)
        maxH = h;
    }

  int vi = 0;
  for (int zy = 0; zy < resZ; zy++)
    for (int zx = 0; zx < resX; zx++) {
      float x0 = zx * stepX - 0.5f, x1 = (zx + 1) * stepX - 0.5f;
      float z0 = zy * stepZ - 0.5f, z1 = (zy + 1) * stepZ - 0.5f;
      float h00 = heights[zy * (resX + 1) + zx];
      float h10 = heights[zy * (resX + 1) + zx + 1];
      float h01 = heights[(zy + 1) * (resX + 1) + zx];
      float h11 = heights[(zy + 1) * (resX + 1) + zx + 1];
      // UV tiling (triplanar-like: 1 UV per world unit)
      float u0 = x0, u1 = x1, v0 = z0, v1 = z1;

      // Triangle 1: (00, 10, 01)
      Vector3 p0 = {x0, h00, z0}, p1 = {x1, h10, z0}, p2 = {x0, h01, z1};
      Vector3 e1 = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
      Vector3 e2 = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
      Vector3 n1 = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                    e1.x * e2.y - e1.y * e2.x};
      float nl = sqrtf(n1.x * n1.x + n1.y * n1.y + n1.z * n1.z);
      if (nl > 0.001f) {
        n1.x /= nl;
        n1.y /= nl;
        n1.z /= nl;
      }

      Vector3 pts1[3] = {p0, p1, p2};
      float uvs1[6] = {u0, v0, u1, v0, u0, v1};
      for (int k = 0; k < 3; k++) {
        mesh.vertices[vi * 3] = pts1[k].x;
        mesh.vertices[vi * 3 + 1] = pts1[k].y;
        mesh.vertices[vi * 3 + 2] = pts1[k].z;
        mesh.normals[vi * 3] = n1.x;
        mesh.normals[vi * 3 + 1] = n1.y;
        mesh.normals[vi * 3 + 2] = n1.z;
        mesh.texcoords[vi * 2] = uvs1[k * 2] * 0.25f;
        mesh.texcoords[vi * 2 + 1] = uvs1[k * 2 + 1] * 0.25f;
        // Sun lighting baked into vertex color
        float sunDot = Clamp(n1.x * (-SUN_DIR.x) + n1.y * (-SUN_DIR.y) +
                                 n1.z * (-SUN_DIR.z),
                             0, 1);
        float light = 0.55f + 0.45f * sunDot;
        // Ambient occlusion style valley darkening / crest brightening
        float yNorm = (pts1[k].y - minH) / (maxH - minH + 1e-5f);
        float ao = 0.82f + (1.12f - 0.82f) * yNorm;
        light *= ao;
        mesh.colors[vi * 4 + 0] = (unsigned char)(210 * light);
        mesh.colors[vi * 4 + 1] = (unsigned char)(185 * light);
        mesh.colors[vi * 4 + 2] = (unsigned char)(140 * light);
        mesh.colors[vi * 4 + 3] = 255;
        vi++;
      }

      // Triangle 2: (10, 11, 01)
      Vector3 p3 = {x1, h11, z1};
      Vector3 e3 = {p3.x - p1.x, p3.y - p1.y, p3.z - p1.z};
      Vector3 e4 = {p2.x - p1.x, p2.y - p1.y, p2.z - p1.z};
      Vector3 n2 = {e3.y * e4.z - e3.z * e4.y, e3.z * e4.x - e3.x * e4.z,
                    e3.x * e4.y - e3.y * e4.x};
      nl = sqrtf(n2.x * n2.x + n2.y * n2.y + n2.z * n2.z);
      if (nl > 0.001f) {
        n2.x /= nl;
        n2.y /= nl;
        n2.z /= nl;
      }

      Vector3 pts2[3] = {p1, p3, p2};
      float uvs2[6] = {u1, v0, u1, v1, u0, v1};
      for (int k = 0; k < 3; k++) {
        mesh.vertices[vi * 3] = pts2[k].x;
        mesh.vertices[vi * 3 + 1] = pts2[k].y;
        mesh.vertices[vi * 3 + 2] = pts2[k].z;
        mesh.normals[vi * 3] = n2.x;
        mesh.normals[vi * 3 + 1] = n2.y;
        mesh.normals[vi * 3 + 2] = n2.z;
        mesh.texcoords[vi * 2] = uvs2[k * 2] * 0.25f;
        mesh.texcoords[vi * 2 + 1] = uvs2[k * 2 + 1] * 0.25f;
        float sunDot = Clamp(n2.x * (-SUN_DIR.x) + n2.y * (-SUN_DIR.y) +
                                 n2.z * (-SUN_DIR.z),
                             0, 1);
        float light = 0.55f + 0.45f * sunDot;
        float yNorm = (pts2[k].y - minH) / (maxH - minH + 1e-5f);
        float ao = 0.82f + (1.12f - 0.82f) * yNorm;
        light *= ao;
        mesh.colors[vi * 4 + 0] = (unsigned char)(210 * light);
        mesh.colors[vi * 4 + 1] = (unsigned char)(185 * light);
        mesh.colors[vi * 4 + 2] = (unsigned char)(140 * light);
        mesh.colors[vi * 4 + 3] = 255;
        vi++;
      }
    }

  UploadMesh(&mesh, false);
  g_terrainModel = LoadModelFromMesh(mesh);
  g_sandTexture = GenerateSandTexture();
  g_sandNormalTexture = GenerateSandNormalTexture();
  g_sandGritTexture = GenerateSandGritTexture();
  g_terrainModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture =
      g_sandTexture;
  g_terrainModel.materials[0].maps[MATERIAL_MAP_NORMAL].texture =
      g_sandNormalTexture;
  g_terrainModel.materials[0].maps[MATERIAL_MAP_SPECULAR].texture =
      g_sandGritTexture;
  g_terrainModel.materials[0].shader = g_shTriplanar;
  float sandScale = 0.25f;
  SetShaderValue(g_shTriplanar, g_locSandScale, &sandScale, SHADER_UNIFORM_FLOAT);
  Vector3 sdir = SUN_DIR;
  SetShaderValue(g_shTriplanar, g_locTripSunDir, &sdir, SHADER_UNIFORM_VEC3);
  g_terrainReady = true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// §7  OVERWORLD — Init / Update / Draw
// ═══════════════════════════════════════════════════════════════════════════════

static void InitOverworld() {
  memset(g_collision, 0, sizeof(g_collision));

  // Player start — fluid continuous position (center of map)
  g_player.posX = (float)(MAP_W / 2);
  g_player.posZ = (float)(MAP_H / 2);
  g_player.velX = 0.0f;
  g_player.velZ = 0.0f;
  g_player.colliderRadius = PLAYER_RADIUS;
  g_player.moving = false;
  g_player.dir = DIR_DOWN;
  g_player.animFrame = 0;
  g_player.animTimer = 0;
  g_player.facingAngle = g_player.targetAngle = DirToAngle(DIR_DOWN);
  g_player.gridX = MAP_W / 2;
  g_player.gridY = MAP_H / 2;
  g_player.lastOverworldPos = {g_player.posX, 0.0f, g_player.posZ};
  g_player.interiorPos = {(float)(SCREEN_W / 2), (float)(SCREEN_H / 2)};

  // Place tents (4×4 AABB cubes with 1×1 doors, colored trim)
  PlaceTent(5, 3, {180, 140, 90, 255}, {160, 40, 40, 255}, "Fatima's Tent",
            SCENE_TENT1); // Crimson roof
  PlaceTent(28, 3, {160, 150, 120, 255}, {50, 80, 170, 255}, "Scholar's Study",
            SCENE_TENT2); // Azure roof
  PlaceTent(5, 20, {170, 120, 80, 255}, {190, 160, 40, 255}, "Yara's Bazaar",
            SCENE_TENT3); // Gold roof
  PlaceTent(28, 20, {150, 135, 100, 255}, {80, 140, 80, 255}, "Kai's Workshop",
            SCENE_TENT4); // Forest roof

  // NPC placement (near their tents, facing south toward player approach)
  g_npcs[0].gx = 8;  g_npcs[0].gy = 8;
  g_npcs[0].worldX = 8.0f;  g_npcs[0].worldZ = 8.0f;
  g_npcs[0].name = "Fatima";
  g_npcs[0].shirtCol = {180, 140, 60, 255};
  g_npcs[0].pantsCol = {90, 70, 50, 255};
  g_npcs[0].hatCol = {180, 140, 60, 255};
  g_npcs[0].dir = DIR_DOWN;
  g_npcs[0].facingAngle = g_npcs[0].targetAngle = DirToAngle(DIR_DOWN);
  g_npcs[0].colors = MakeChibiColors(
      {180, 140, 100, 255}, {60, 40, 25, 255}, {180, 140, 60, 255},
      {140, 100, 50, 255}, {90, 70, 50, 255}, {160, 120, 60, 255});

  g_npcs[1].gx = 31; g_npcs[1].gy = 8;
  g_npcs[1].worldX = 31.0f; g_npcs[1].worldZ = 8.0f;
  g_npcs[1].name = "Scholar";
  g_npcs[1].shirtCol = {60, 160, 160, 255};
  g_npcs[1].pantsCol = {180, 180, 170, 255};
  g_npcs[1].hatCol = {60, 160, 160, 255};
  g_npcs[1].dir = DIR_DOWN;
  g_npcs[1].facingAngle = g_npcs[1].targetAngle = DirToAngle(DIR_DOWN);
  g_npcs[1].colors = MakeChibiColors(
      {140, 100, 70, 255}, {30, 25, 20, 255}, {60, 160, 160, 255},
      {200, 220, 220, 255}, {180, 180, 170, 255}, {100, 80, 60, 255});

  g_npcs[2].gx = 8;  g_npcs[2].gy = 25;
  g_npcs[2].worldX = 8.0f;  g_npcs[2].worldZ = 25.0f;
  g_npcs[2].name = "Yara";
  g_npcs[2].shirtCol = {80, 30, 30, 255};
  g_npcs[2].pantsCol = {50, 45, 40, 255};
  g_npcs[2].hatCol = {100, 30, 30, 255};
  g_npcs[2].dir = DIR_DOWN;
  g_npcs[2].facingAngle = g_npcs[2].targetAngle = DirToAngle(DIR_DOWN);
  g_npcs[2].colors = MakeChibiColors(
      {160, 120, 80, 255}, {40, 30, 20, 255}, {100, 30, 30, 255},
      {80, 30, 30, 255}, {50, 45, 40, 255}, {60, 50, 40, 255});

  g_npcs[3].gx = 31; g_npcs[3].gy = 25;
  g_npcs[3].worldX = 31.0f; g_npcs[3].worldZ = 25.0f;
  g_npcs[3].name = "Kai";
  g_npcs[3].shirtCol = {80, 140, 60, 255};
  g_npcs[3].pantsCol = {120, 110, 80, 255};
  g_npcs[3].hatCol = {80, 140, 60, 255};
  g_npcs[3].dir = DIR_LEFT;
  g_npcs[3].facingAngle = g_npcs[3].targetAngle = DirToAngle(DIR_LEFT);
  g_npcs[3].colors = MakeChibiColors(
      {200, 160, 120, 255}, {50, 35, 20, 255}, {80, 140, 60, 255},
      {140, 160, 100, 255}, {120, 110, 80, 255}, {140, 100, 50, 255});

  for (int i = 0; i < 4; i++)
    g_collision[g_npcs[i].gy][g_npcs[i].gx] = 1;

  // Procedural rocks
  InitProceduralRocks();

  // Generate dune terrain mesh
  GenerateTerrainMesh();

  // Camera (2.5D isometric-ish angle)
  g_cam.position = {g_player.posX, 14.0f, g_player.posZ + 10.0f};
  g_cam.target = {g_player.posX, GetDuneHeight(g_player.posX, g_player.posZ), g_player.posZ};
  g_cam.up = {0, 1, 0};
  g_cam.fovy = 40.0f;
  g_cam.projection = CAMERA_PERSPECTIVE;
}

// ── STRICTLY ORTHOGONAL movement — NO diagonal, smooth 0.2s Lerp ────────────
// ── Onboarding dialog text ──────────────────────────────────────────────────
static const char *g_onboardingTexts[] = {
    "Welcome to Yellow Village, young trader!\nI'm Grandpa Aziz. The desert is "
    "full\nof card traders and treasure...",
    "Here, take this starter deck.\nIt has 30 basic cards to get you going.",
    "Talk to NPCs to challenge them to duels.\nWin matches to earn coins!",
    "Visit the Bazaar tent to buy card packs\nand sell cards at market prices.",
    "Prices change throughout the day and\nbased on supply and demand. Trade "
    "wisely!",
    "When you're ready, enter tournaments\nto advance through the leagues!",
};
static constexpr int NUM_ONBOARDING_TEXTS = 6;
static bool g_onboardingActive = false;
static int g_onboardingTextIdx = 0;

static int MapWalk6ToTex3(int frame6) {
  static const int map[6] = {0, 1, 2, 1, 2, 1};
  return map[frame6 % 6];
}

static int GetWalkFrame6(float timer) { return ((int)(timer / 0.085f)) % 6; }

static int GetIdleFrame2(float timer) { return ((int)(timer / 0.45f)) % 2; }

// ═══════════════════════════════════════════════════════════════════════════════
// FLUID MOVEMENT SYSTEM — Vector-based velocity, sphere collision, wall sliding
// ═══════════════════════════════════════════════════════════════════════════════

// Check if a circle at (cx, cz) with radius r overlaps any solid tile in the
// collision map. Returns true if blocked. Used for continuous sphere collider.
static bool CircleCollidesWorld(float cx, float cz, float r) {
  // Check all tiles the circle could overlap (bounding box of circle)
  int minTX = (int)floorf(cx - r);
  int maxTX = (int)floorf(cx + r);
  int minTZ = (int)floorf(cz - r);
  int maxTZ = (int)floorf(cz + r);
  for (int tz = minTZ; tz <= maxTZ; tz++) {
    for (int tx = minTX; tx <= maxTX; tx++) {
      // Out of bounds = solid wall
      if (tx < 0 || tx >= MAP_W || tz < 0 || tz >= MAP_H) {
        // Circle vs AABB (tile is [tx, tx+1] × [tz, tz+1])
        float nearX = Clamp(cx, (float)tx, (float)(tx + 1));
        float nearZ = Clamp(cz, (float)tz, (float)(tz + 1));
        float dx = cx - nearX, dz = cz - nearZ;
        if (dx * dx + dz * dz < r * r) return true;
        continue;
      }
      if (g_collision[tz][tx] == 0) continue;
      // Solid tile — circle vs AABB overlap test
      float nearX = Clamp(cx, (float)tx, (float)(tx + 1));
      float nearZ = Clamp(cz, (float)tz, (float)(tz + 1));
      float dx = cx - nearX, dz = cz - nearZ;
      if (dx * dx + dz * dz < r * r) return true;
    }
  }
  // NPC sphere collision (each NPC is a cylinder of radius 0.4)
  for (int i = 0; i < 4; i++) {
    float dx = cx - g_npcs[i].worldX;
    float dz = cz - g_npcs[i].worldZ;
    float minDist = r + 0.4f;
    if (dx * dx + dz * dz < minDist * minDist) return true;
  }
  return false;
}

// Push the circle out of any solid tiles it overlaps. Returns resolved position.
// This enables "wall sliding" — if you walk diagonally into a wall, you slide
// along it instead of stopping dead.
static void ResolveCircleCollision(float &cx, float &cz, float r) {
  for (int iter = 0; iter < 4; iter++) { // max 4 resolution iterations
    int minTX = (int)floorf(cx - r);
    int maxTX = (int)floorf(cx + r);
    int minTZ = (int)floorf(cz - r);
    int maxTZ = (int)floorf(cz + r);
    bool resolved = true;
    for (int tz = minTZ; tz <= maxTZ; tz++) {
      for (int tx = minTX; tx <= maxTX; tx++) {
        bool solid = false;
        if (tx < 0 || tx >= MAP_W || tz < 0 || tz >= MAP_H) {
          solid = true;
        } else if (g_collision[tz][tx] != 0) {
          solid = true;
        }
        if (!solid) continue;
        // Circle vs AABB — find nearest point on tile
        float nearX = Clamp(cx, (float)tx, (float)(tx + 1));
        float nearZ = Clamp(cz, (float)tz, (float)(tz + 1));
        float dx = cx - nearX, dz = cz - nearZ;
        float dist2 = dx * dx + dz * dz;
        if (dist2 < r * r && dist2 > 0.0001f) {
          float dist = sqrtf(dist2);
          float push = r - dist + 0.001f;
          cx += (dx / dist) * push;
          cz += (dz / dist) * push;
          resolved = false;
        }
      }
    }
    // NPC push-out
    for (int i = 0; i < 4; i++) {
      float dx = cx - g_npcs[i].worldX;
      float dz = cz - g_npcs[i].worldZ;
      float minDist = r + 0.4f;
      float dist2 = dx * dx + dz * dz;
      if (dist2 < minDist * minDist && dist2 > 0.0001f) {
        float dist = sqrtf(dist2);
        float push = minDist - dist + 0.001f;
        cx += (dx / dist) * push;
        cz += (dz / dist) * push;
        resolved = false;
      }
    }
    if (resolved) break;
  }
}

// ── Update player velocity from input, apply movement with collision ─────────
// Returns the velocity vector (for facing angle computation).
static Vector3 UpdatePlayerVelocity(float dt) {
  // Read analog input (keyboard simulates ±1.0, gamepad gives true analog)
  float inputX = 0.0f, inputZ = 0.0f;
  if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) inputX += 1.0f;
  if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))  inputX -= 1.0f;
  if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))  inputZ += 1.0f;
  if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))    inputZ -= 1.0f;
  // Gamepad analog stick (additive, clamped)
  inputX = Clamp(inputX + g_dpadX, -1.0f, 1.0f);
  inputZ = Clamp(inputZ + g_dpadY, -1.0f, 1.0f);

  // Normalize so diagonal speed == cardinal speed
  float mag = sqrtf(inputX * inputX + inputZ * inputZ);
  Vector3 vel = {0, 0, 0};
  if (mag > 0.15f) { // deadzone
    float nx = inputX / mag;
    float nz = inputZ / mag;
    vel = {nx * PLAYER_SPEED, 0, nz * PLAYER_SPEED};
  }

  g_player.velX = vel.x;
  g_player.velZ = vel.z;
  g_player.moving = (fabsf(vel.x) + fabsf(vel.z)) > 0.01f;

  if (g_player.moving) {
    // Apply velocity
    float newX = g_player.posX + vel.x * dt;
    float newZ = g_player.posZ + vel.z * dt;
    // Resolve collisions with wall sliding
    ResolveCircleCollision(newX, newZ, g_player.colliderRadius);
    g_player.posX = newX;
    g_player.posZ = newZ;
    // Snap grid coords for legacy collision map compat
    g_player.gridX = (int)roundf(g_player.posX);
    g_player.gridY = (int)roundf(g_player.posZ);
  }
  return vel;
}

static bool CheckOverworldTileOccupancy(int gx, int gy) {
  if (gx < 0 || gx >= MAP_W || gy < 0 || gy >= MAP_H)
    return true;
  if (g_collision[gy][gx] != 0)
    return true;
  for (int i = 0; i < 4; i++) {
    int nx = (int)roundf(g_npcs[i].worldX);
    int ny = (int)roundf(g_npcs[i].worldZ);
    if (nx == gx && ny == gy)
      return true;
  }
  Vector3 p = {(float)gx, 1.0f, (float)gy};
  for (int i = 0; i < g_numTents; i++) {
    const TentInstance &t = g_tentInstances[i];
    bool inDoor = (p.x >= t.doorAABB.min.x && p.x <= t.doorAABB.max.x &&
                   p.y >= t.doorAABB.min.y && p.y <= t.doorAABB.max.y &&
                   p.z >= t.doorAABB.min.z && p.z <= t.doorAABB.max.z);
    bool inWall = (p.x >= t.wallAABB.min.x && p.x <= t.wallAABB.max.x &&
                   p.y >= t.wallAABB.min.y && p.y <= t.wallAABB.max.y &&
                   p.z >= t.wallAABB.min.z && p.z <= t.wallAABB.max.z);
    if (inWall && !inDoor)
      return true;
  }
  return false;
}

static void UpdateOverworld(float dt) {
  UpdateController(dt);
  g_time += dt;
  UpdateTentVisibility();

  // ── Onboarding sequence (first time only) ──────────────────────────────
  if (!g_onboardingDone) {
    if (!g_onboardingActive) {
      g_onboardingActive = true;
      g_onboardingTextIdx = 0;
    }
    if (IsKeyPressed(KEY_ENTER) || g_padAPressed ||
        IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
      g_onboardingTextIdx++;
      if (g_onboardingTextIdx == 2 && !g_hasStarterDeck) {
        // Give starter deck at step 2
        GiveStarterDeck();
        g_inventory.Give(ITEM_KINGDOM_MAP); // starter item
      }
      if (g_onboardingTextIdx >= NUM_ONBOARDING_TEXTS) {
        g_onboardingDone = true;
        g_onboardingActive = false;
        g_grandpaTutorialDone = true;
      }
    }
    // During onboarding, skip normal movement but still update visuals
    UpdateVFX(dt);
    // Camera follow
    g_cam.position = {g_player.posX, 14.0f, g_player.posZ + 10.0f};
    g_cam.target = {g_player.posX, GetDuneHeight(g_player.posX, g_player.posZ), g_player.posZ};
    return;
  }

  // Screen-space particles (camera-independent wind)
  UpdateVFX(dt);

  // ── FLUID MOVEMENT: vector-based velocity with diagonal normalization ─────
  Vector3 vel = {0, 0, 0};
  if (!g_npcDialogOpen && !g_menuOpen) {
    vel = UpdatePlayerVelocity(dt);
  } else {
    g_player.moving = false;
    g_player.velX = g_player.velZ = 0;
  }

  // Animation timer
  g_player.animTimer += dt;
  if (g_player.moving) {
    int f6 = GetWalkFrame6(g_player.animTimer);
    g_player.animFrame = MapWalk6ToTex3(f6);
  } else {
    int i2 = GetIdleFrame2(g_player.animTimer);
    g_player.animFrame = i2 % FRAMES_PER_DIR;
  }

  // ── FACING: Compute target angle from velocity vector ─────────────────────
  // Gen-5 rule: UP/NORTH → back faces camera (180°), DOWN/SOUTH → front (0°)
  // The angle is atan2 of the velocity mapped so +Z (south) = 0°.
  if (g_player.moving) {
    // atan2(velX, velZ) gives angle where +Z=0°, +X=90° (clockwise from south)
    // This naturally maps: moving south (0,+z)=0°, north (0,-z)=180°,
    //   east (+x,0)=270° (model right = screen right), west(-x,0)=90°
    float rawAngle = atan2f(vel.x, vel.z) * RAD2DEG;
    // Wrap to 0-360
    if (rawAngle < 0) rawAngle += 360.0f;
    g_player.targetAngle = rawAngle;
    // Update cardinal dir hint from dominant axis
    float ax = fabsf(vel.x), az = fabsf(vel.z);
    if (az >= ax) {
      g_player.dir = (vel.z > 0) ? DIR_DOWN : DIR_UP;
    } else {
      g_player.dir = (vel.x > 0) ? DIR_RIGHT : DIR_LEFT;
    }
  }
  // Smooth rotation (0.15s slerp, never snaps)
  UpdateFacingAngle(g_player.facingAngle, g_player.targetAngle, dt);

  // NPC rotation: smoothly turn toward player when close
  for (int i = 0; i < 4; i++) {
    Dir npcFaceDir = GetNPCPlayerDir({g_npcs[i].worldX, 0, g_npcs[i].worldZ});
    g_npcs[i].targetAngle = DirToAngle(npcFaceDir);
    UpdateFacingAngle(g_npcs[i].facingAngle, g_npcs[i].targetAngle, dt);
  }

  // ── Tent "Ceiling Hide" — handled by UpdateTentVisibility() at frame start.

  // ── NPC interaction: Enter/E key, proximity 1.5 units ──────────────────
  if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_E) || g_padAPressed) &&
      !g_npcDialogOpen && !g_menuOpen) {
    // Find nearest NPC within interaction radius
    float bestDist = 1.5f;
    int bestNPC = -1;
    for (int i = 0; i < 4; i++) {
      float ddx = g_npcs[i].worldX - g_player.posX;
      float ddz = g_npcs[i].worldZ - g_player.posZ;
      float dist = sqrtf(ddx * ddx + ddz * ddz);
      if (dist < bestDist) {
        bestDist = dist;
        bestNPC = i;
      }
    }
    if (bestNPC >= 0) InteractWithNPC(bestNPC);
  }

  // Menu / dialog
  if (IsKeyPressed(KEY_SPACE) || g_padStartPressed)
    g_menuOpen = !g_menuOpen;
  if (IsKeyPressed(KEY_ESCAPE) || g_padBPressed) {
    g_menuOpen = false;
    g_npcDialogOpen = false;
    g_dialogPhase = DIALOG_MENU;
    g_dialogText = nullptr;
  }
  if (g_npcDialogOpen)
    UpdateNPCDialog();
  if (g_menuOpen)
    UpdateMenu();

  // Smooth camera follow
  float camLerp = Clamp(dt * 5.0f, 0, 1);
  g_cam.position.x = Lerp(g_cam.position.x, g_player.posX, camLerp);
  g_cam.position.z = Lerp(g_cam.position.z, g_player.posZ + 10.0f, camLerp);
  g_cam.position.y = 14.0f;
  g_cam.target.x = Lerp(g_cam.target.x, g_player.posX, camLerp);
  g_cam.target.z = Lerp(g_cam.target.z, g_player.posZ, camLerp);
  float groundY = GetDuneHeight(g_player.posX, g_player.posZ);
  g_cam.target.y = Lerp(g_cam.target.y, groundY, camLerp);
}

// Returns the NPC facing direction based on PLAYER proximity.
// Within 3 units: NPC 3D model smoothly rotates to face player.
// Beyond 3 units: default DIR_DOWN (facing south).
static Dir GetNPCPlayerDir(Vector3 npcPos) {
  float dx = g_player.posX - npcPos.x;
  float dz = g_player.posZ - npcPos.z;
  float dist = sqrtf(dx * dx + dz * dz);
  if (dist < 3.0f) {
    float ax = fabsf(dx), az = fabsf(dz);
    if (az >= ax) return (dz > 0.0f) ? DIR_DOWN : DIR_UP;
    return (dx > 0.0f) ? DIR_RIGHT : DIR_LEFT;
  }
  return DIR_DOWN;
}

// [DELETED: DrawFixedSprite — replaced by DrawChibiModel3D]

static void DrawSpriteBlobShadow(Vector3 basePos, float radius) {
  float gy = GetDuneHeight(basePos.x, basePos.z) + 0.012f;
  DrawCylinder({basePos.x, gy, basePos.z}, radius * 1.05f, radius * 1.05f, 0.008f,
               14, {0, 0, 0, 102});
  DrawCylinder({basePos.x, gy + 0.001f, basePos.z}, radius * 0.72f, radius * 0.72f,
               0.008f, 14, {0, 0, 0, 58});
}

static void DrawOverworld() {
  // ── Render 3D scene into FBO for post-processing ────────────────────────
  BeginTextureMode(g_sceneFBO);
  // Day/night tinted sky
  Color ambBase = g_worldClock.GetAmbientTint();
  Color ambWarm = {220, 180, 140, 255};
  Color amb = {(unsigned char)Clamp(ambBase.r * 0.6f + ambWarm.r * 0.4f, 0, 255),
               (unsigned char)Clamp(ambBase.g * 0.6f + ambWarm.g * 0.4f, 0, 255),
               (unsigned char)Clamp(ambBase.b * 0.6f + ambWarm.b * 0.4f, 0, 255),
               255};
  ClearBackground({(unsigned char)(70 * amb.r / 255),
                   (unsigned char)(55 * amb.g / 255),
                   (unsigned char)(35 * amb.b / 255), 255});

  BeginMode3D(g_cam);
  {
    // Update triplanar shader view-dependent uniforms (rim light needs camera)
    Vector3 camPos = g_cam.position;
    SetShaderValue(g_shTriplanar, g_locTripCameraPos, &camPos, SHADER_UNIFORM_VEC3);
    // ── PASS 1: Opaque terrain (dune heightmap mesh) ─────────────────────
    if (g_terrainReady) {
      DrawModel(g_terrainModel, {0, 0, 0}, 1.0f, amb);
    }

    // ── PASS 2: Opaque environment (tents, rocks) ───────────────────────
    // Tent shadows (PCF soft)
    for (int i = 0; i < g_numTents; i++)
      DrawTentShadow(g_tentInstances[i]);

    // Character shadows
    float playerY = GetDuneHeight(g_player.posX, g_player.posZ);
    DrawSpriteBlobShadow({g_player.posX, playerY, g_player.posZ}, 0.5f);
    for (int i = 0; i < 4; i++) {
      float ny = GetDuneHeight(g_npcs[i].worldX, g_npcs[i].worldZ);
      DrawSpriteBlobShadow({g_npcs[i].worldX, ny, g_npcs[i].worldZ}, 0.42f);
    }

    // Rock shadows
    for (int i = 0; i < g_numRocks; i++) {
      RockInstance &ri = g_rockInstances[i];
      float ry = GetDuneHeight(ri.x, ri.z);
      DrawSoftShadow({ri.x, ry, ri.z}, ri.scale * 1.5f, ri.scale * 2.0f);
    }

    // Draw tents (poles, roof, walls, sign all inside DrawTent now)
    for (int i = 0; i < g_numTents; i++)
      DrawTent(g_tentInstances[i]);

    // Draw procedural rocks (placed on dune surface)
    for (int i = 0; i < g_numRocks; i++) {
      RockInstance &ri = g_rockInstances[i];
      float ry = GetDuneHeight(ri.x, ri.z);
      DrawModelEx(ri.model, {ri.x, ry, ri.z}, {0, 1, 0}, ri.rotY,
                  {ri.scale, ri.scale, ri.scale}, WHITE);
    }

    // ── PASS 3: 3D Chibi Characters (BDSP vinyl figure style) ─────────────
    // NPCs — 3D models with smooth rotation toward player
    for (int i = 0; i < 4; i++) {
      float npcY = GetDuneHeight(g_npcs[i].worldX, g_npcs[i].worldZ);
      Vector3 npcPos = {g_npcs[i].worldX, npcY, g_npcs[i].worldZ};
      DrawChibiModel3D(npcPos, g_npcs[i].facingAngle, g_npcs[i].colors,
                       g_time + i * 0.22f, false, 0.6f);
    }

    // Player — 3D model with walk animation
    {
      float py = GetDuneHeight(g_player.posX, g_player.posZ);
      Vector3 playerPos = {g_player.posX, py, g_player.posZ};
      DrawChibiModel3D(playerPos, g_player.facingAngle, g_playerColors,
                       g_player.animTimer, g_player.moving, 0.8f);
    }
  }
  EndMode3D();
  EndTextureMode();

  // ── Full post-processing (composite renders into g_pixelFBO) ───────────
  ApplyFullPostProcess(g_time);

  // ── Screen-space wind lines (rendering into g_pixelFBO) ──────────────────
  DrawVFX();

  BeginShaderMode(g_shSandstorm);
  SetShaderValue(g_shSandstorm, g_locStormTime, &g_time, SHADER_UNIFORM_FLOAT);
  float stormRes[2] = {(float)SCREEN_W, (float)SCREEN_H};
  SetShaderValue(g_shSandstorm, g_locStormRes, stormRes, SHADER_UNIFORM_VEC2);
  DrawRectangle(0, 0, SCREEN_W, SCREEN_H, {255, 255, 255, 20});
  EndShaderMode();

  EndTextureMode(); // end g_pixelFBO

  // ── PIXELATION PASS — draw g_pixelFBO through pixel-art downscale shader ─
  if (g_highFidelityMode) {
    DrawFBOQuad(g_pixelFBO);
  } else {
    float pixelRes[2] = {320.0f, 213.0f};
    BeginShaderMode(g_shPixelate);
    SetShaderValue(g_shPixelate, g_locPixelRes, pixelRes, SHADER_UNIFORM_VEC2);
    DrawFBOQuad(g_pixelFBO);
    EndShaderMode();
  }

  // ── HUD (2D overlay) ─────────────────────────────────────────────────────
  DrawText("WASD: Move  E/Enter: Talk  Space: Menu  Esc: Back", 10, SCREEN_H - 25,
           14, {180, 160, 120, 200});
  char posStr[64];
  snprintf(posStr, 64, "[%d,%d]", g_player.gridX, g_player.gridY);
  DrawText(posStr, SCREEN_W - 80, 10, 12, {180, 160, 120, 180});

  // Coins & time of day
  char hudBuf[128];
  snprintf(hudBuf, 128, "Coins: %d", g_playerCoins);
  DrawText(hudBuf, 10, 10, 14, {255, 220, 120, 240});
  snprintf(hudBuf, 128, "%s", g_worldClock.GetName());
  DrawText(hudBuf, 10, 28, 12, {200, 180, 140, 200});

  // Tournament progress
  snprintf(hudBuf, 128, "%s - %s League (%d/%d)", g_tournament.GetCityName(),
           g_tournament.GetLeagueName(), g_tournament.roundsWon,
           g_tournament.roundsPerLeague);
  DrawText(hudBuf, 10, 44, 10, {180, 170, 140, 180});

  // Onboarding dialog overlay
  if (g_onboardingActive && !g_onboardingDone &&
      g_onboardingTextIdx < NUM_ONBOARDING_TEXTS) {
    // BW-style dialog box at bottom of screen
    DrawRectangle(20, SCREEN_H - 130, SCREEN_W - 40, 110, {20, 15, 10, 230});
    DrawRectangleLinesEx(
        {20, (float)(SCREEN_H - 130), (float)(SCREEN_W - 40), 110}, 2.0f,
        {200, 170, 100, 255});
    DrawText("Grandpa Aziz", 40, SCREEN_H - 125, 14, {255, 220, 120, 255});
    DrawText(g_onboardingTexts[g_onboardingTextIdx], 40, SCREEN_H - 105, 14,
             {230, 220, 190, 255});
    DrawText("[Enter] Continue", SCREEN_W - 160, SCREEN_H - 35, 10,
             {180, 160, 120, 180});
  }

  // NPC dialog overlay
  if (g_npcDialogOpen)
    DrawNPCDialog();
  if (g_menuOpen)
    DrawMenuOverlay();
}

// TentInterior methods — no-op stubs (seamless overworld cutaway replaces them)
void TentInterior::Update(float dt) { (void)dt; }
void TentInterior::Draw() {}

// ═══════════════════════════════════════════════════════════════════════════════
// MAIN ENTRY POINT
// ═══════════════════════════════════════════════════════════════════════════════
int main() {
  SetConfigFlags(FLAG_MSAA_4X_HINT);
  InitWindow(SCREEN_W, SCREEN_H, "Sovereign Horizons — KOG 2.5D Editor");
  SetTargetFPS(60);
  SetExitKey(0); // Disable default ESC=quit so we can use ESC for menus

  // Init systems
  InitPostProcessing();
  GenerateAllCardTextures();
  InitOverworld();
  InitCharacterColors(); // 3D chibi color palettes (no more sprite textures)
  g_market.Init();
  g_tournament.Init();
  g_worldClock.Init();
  g_inventory.Init();

  // Main loop
  while (!WindowShouldClose()) {
    float dt = GetFrameTime();

    // ── Update ──────────────────────────────────────────────────────────
    g_worldClock.Update(dt);
    g_sceneManager.Update(dt);

    if (!g_sceneManager.IsTransitioning()) {
      switch (g_scene) {
      case SCENE_OVERWORLD:
        UpdateOverworld(dt);
        break;
      case SCENE_MATCH:
        UpdateMatch(dt);
        break;
      case SCENE_SHOP:
        UpdateShop(dt);
        break;
      default:
        break;
      }
    }

    // ── Draw ────────────────────────────────────────────────────────────
    BeginDrawing();
    ClearBackground({30, 22, 15, 255});

    switch (g_scene) {
    case SCENE_OVERWORLD:
      DrawOverworld();
      break;
    case SCENE_MATCH:
      DrawMatchScene();
      break;
    case SCENE_SHOP:
      DrawShopScene();
      break;
    default:
      DrawOverworld();
      break;
    }

    // FPS counter (debug)
    DrawFPS(SCREEN_W - 80, SCREEN_H - 20);
    g_sceneManager.DrawFadeOverlay();
    EndDrawing();
  }

  // Cleanup (3D chibi models are procedural — no textures to unload)
  for (int i = 0; i < g_numRocks; i++)
    UnloadModel(g_rockInstances[i].model);
  CleanupPostProcessing();
  CloseWindow();
  return 0;
}
