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
    vec2 delta=(uv-sunPos)*(1.0/16.0)*0.98;
    vec4 s=vec4(0.0); float decay=1.0;
    for(int i=0;i<16;i++){
        uv-=delta;
        s+=texture(texture0,clamp(uv,0.0,1.0))*decay*0.055;
        decay*=0.93;
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
        for(int i=0;i<4;i++){
            float a=float(i)*1.5707963;
            sum+=texture(texture0,uv+vec2(cos(a),sin(a))*blurR).rgb;
        }
        scene=sum/5.0;
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
    float dust=n1*0.60+n2*0.40;
    dust=smoothstep(0.30,0.80,dust);
    dust*=0.75;
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
    // Smooth sun/shadow gradient — warm sandy shadow, never black
    float L = max(dot(n, -sunDir), 0.0);
    float sunLit    = mix(0.60, 1.0, smoothstep(0.10, 0.75, L));
    vec3 sunCol     = vec3(1.0, 0.820, 0.643);    // #FFD1A4 warm golden-orange
    vec3 shadowCol  = vec3(0.58, 0.40, 0.22);     // warm sandy shadow tint
    vec3 lighting   = mix(shadowCol, sunCol, sunLit);
    // Rim highlight on crests (view-dependent)
    vec3 V = normalize(cameraPos - fragWorldPos);
    float rim = pow(clamp(1.0 - max(dot(n, V), 0.0), 0.0, 1.0), 3.0);
    rim *= smoothstep(0.6, 0.95, L); // only on lit side near crest
    vec3 rimCol = vec3(1.15, 1.05, 0.85);
    // Pixel-grit overlay (two-tone), low frequency to avoid moiré
    float gritMask = grit.r;
    vec3 gritCol = mix(vec3(0.92, 0.80, 0.60), vec3(0.70, 0.55, 0.40), step(0.5, gritMask));
    vec3 color = base * lighting;
    color = mix(color, color * rimCol, rim * 0.35);
    color = mix(color, gritCol, 0.08);
    // colDiffuse tint only — vertex-color AO removed (was darkening shadows to black)
    color *= colDiffuse.rgb;
    finalColor = vec4(color, 1.0);
}
)";
// ── City shaders ─────────────────────────────────────────────────────────────
static const char *VS_GOLD = R"(
#version 330
layout(location=0) in vec3 vertexPosition;
layout(location=2) in vec3 vertexNormal;
layout(location=3) in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
out vec3 fragPos;
out vec3 fragNormal;
void main(){
    fragPos    = vec3(matModel * vec4(vertexPosition,1.0));
    fragNormal = normalize(vec3(matModel * vec4(vertexNormal,0.0)));
    gl_Position = mvp * vec4(vertexPosition,1.0);
}
)";

static const char *FS_GOLD = R"(
#version 330
in vec3 fragPos;
in vec3 fragNormal;
uniform vec4 colDiffuse;
uniform float time;
uniform vec3 cameraPos;
uniform vec3 sunDir;
out vec4 finalColor;
void main(){
    vec3 n = normalize(fragNormal);
    vec3 v = normalize(cameraPos - fragPos);
    vec3 l = normalize(-sunDir);
    vec3 h = normalize(l + v);
    vec3 goldBase = vec3(0.87, 0.65, 0.15);
    vec3 goldHot  = vec3(1.00, 0.93, 0.56);
    float NdotL  = max(dot(n, l), 0.0);
    float spec   = pow(max(dot(n, h), 0.0), 90.0);
    float F0     = 0.92;
    float fresnel = F0 + (1.0-F0)*pow(1.0-max(dot(n,v),0.0),5.0);
    float shimmer = 1.0 + 0.04*sin(time*1.9 + fragPos.x*0.4 + fragPos.z*0.3);
    vec3 col = goldBase*(0.25 + 0.75*NdotL)
             + goldHot*spec*shimmer*0.85
             + goldHot*(fresnel-F0)*0.28;
    col = clamp(col * colDiffuse.rgb/255.0, 0.0, 1.0);
    finalColor = vec4(col, 1.0);
}
)";

static const char *FS_WATER = R"(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float time;
out vec4 finalColor;
void main(){
    vec2 uv = fragTexCoord;
    float wave1 = sin(uv.x*7.2 + time*1.3)*0.013;
    float wave2 = cos(uv.y*5.8 + time*0.95)*0.013;
    vec2 warp = clamp(uv + vec2(wave1,wave2), 0.0, 1.0);
    vec3 deep    = vec3(0.04, 0.14, 0.27);
    vec3 shallow = vec3(0.18, 0.52, 0.65);
    vec3 glint   = vec3(0.80, 0.95, 1.00);
    float depth = length(uv - 0.5)*2.0;
    vec3 col = mix(deep, shallow, smoothstep(0.0, 1.0, depth));
    float caus = pow(0.5 + 0.5*sin(warp.x*11.0+time*2.4)*cos(warp.y*9.0+time*1.8), 4.0);
    col += glint*caus*0.30;
    float spark = pow(0.5+0.5*sin(uv.x*44.0+time*8.0)*cos(uv.y*37.0+time*7.2), 18.0);
    col += vec3(1.0)*spark*0.18;
    float rimA = 1.0 - smoothstep(0.3, 0.5, depth);
    float alpha = mix(0.92, 0.75, depth)*rimA + 0.12;
    finalColor = vec4(col, alpha);
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
// ── Frustum culling ───────────────────────────────────────────────────────────
static float g_frustum[6][4]; // [plane][a,b,c,d]  (normals pointing inward)

// Called once per frame inside BeginMode3D, after camera matrices are loaded.
static void UpdateFrustumPlanes() {
  Matrix mv   = rlGetMatrixModelview();
  Matrix proj = rlGetMatrixProjection();
  // clip = proj * mv  (transforms world → clip space)
  Matrix c = MatrixMultiply(proj, mv);
  // Gribb-Hartmann plane extraction (column-major Raylib Matrix)
  // Row i of c: {c.m[i], c.m[i+4], c.m[i+8], c.m[i+12]}
  // Left   = row3 + row0
  g_frustum[0][0]=c.m3+c.m0; g_frustum[0][1]=c.m7+c.m4;
  g_frustum[0][2]=c.m11+c.m8; g_frustum[0][3]=c.m15+c.m12;
  // Right  = row3 - row0
  g_frustum[1][0]=c.m3-c.m0; g_frustum[1][1]=c.m7-c.m4;
  g_frustum[1][2]=c.m11-c.m8; g_frustum[1][3]=c.m15-c.m12;
  // Bottom = row3 + row1
  g_frustum[2][0]=c.m3+c.m1; g_frustum[2][1]=c.m7+c.m5;
  g_frustum[2][2]=c.m11+c.m9; g_frustum[2][3]=c.m15+c.m13;
  // Top    = row3 - row1
  g_frustum[3][0]=c.m3-c.m1; g_frustum[3][1]=c.m7-c.m5;
  g_frustum[3][2]=c.m11-c.m9; g_frustum[3][3]=c.m15-c.m13;
  // Near   = row3 + row2
  g_frustum[4][0]=c.m3+c.m2; g_frustum[4][1]=c.m7+c.m6;
  g_frustum[4][2]=c.m11+c.m10; g_frustum[4][3]=c.m15+c.m14;
  // Far    = row3 - row2
  g_frustum[5][0]=c.m3-c.m2; g_frustum[5][1]=c.m7-c.m6;
  g_frustum[5][2]=c.m11-c.m10; g_frustum[5][3]=c.m15-c.m14;
  // Normalize each plane so d is a signed distance in world units
  for (int i = 0; i < 6; i++) {
    float len = sqrtf(g_frustum[i][0]*g_frustum[i][0]
                    + g_frustum[i][1]*g_frustum[i][1]
                    + g_frustum[i][2]*g_frustum[i][2]);
    if (len > 1e-6f) {
      g_frustum[i][0] /= len; g_frustum[i][1] /= len;
      g_frustum[i][2] /= len; g_frustum[i][3] /= len;
    }
  }
}

// Returns false if sphere is entirely outside the view frustum.
static inline bool SphereInFrustum(float cx, float cy, float cz, float r) {
  for (int i = 0; i < 6; i++) {
    float d = g_frustum[i][0]*cx + g_frustum[i][1]*cy
            + g_frustum[i][2]*cz + g_frustum[i][3];
    if (d < -r) return false;
  }
  return true;
}

// City shaders & primitive models
static Shader g_shGold, g_shWater;
static int    g_locGoldTime, g_locGoldCamPos, g_locGoldSunDir;
static int    g_locWaterTime;
static Model  g_goldModel, g_waterModel;
static bool   g_cityModelsReady = false;

// ── Constants
// ─────────────────────────────────────────────────────────────────
static constexpr int SCREEN_W = 960;
static constexpr int SCREEN_H = 640;
static constexpr int MAP_W = 200;
static constexpr int MAP_H = 150;
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
// ── Five Cities of Eretz ─────────────────────────────────────────────────────
static constexpr float CITY_ZAHAV_X  = 100.0f, CITY_ZAHAV_Z  =  12.0f;
static constexpr float CITY_MAAYAN_X =  22.0f, CITY_MAAYAN_Z =  75.0f;
static constexpr float CITY_AVAK_X   = 100.0f, CITY_AVAK_Z   = 133.0f;
static constexpr float CITY_GAN_X    = 177.0f, CITY_GAN_Z    =  75.0f;
static constexpr float CITY_SELA_X   =  35.0f, CITY_SELA_Z   =  30.0f;
static constexpr float CITY_RADIUS   =  14.0f;
static const float CITY_POS[5][2] = {
  {CITY_ZAHAV_X,  CITY_ZAHAV_Z },
  {CITY_MAAYAN_X, CITY_MAAYAN_Z},
  {CITY_AVAK_X,   CITY_AVAK_Z  },
  {CITY_GAN_X,    CITY_GAN_Z   },
  {CITY_SELA_X,   CITY_SELA_Z  },
};
static const char *CITY_NAMES[] = {
  "",  // Zahav removed
  "Ma'ayan - The Oasis",
  "Avak - The Dust Town",
  "Gan - The Blooming Town",
  "Sela - The Canyon Town"
};

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
  // ── City shaders ──────────────────────────────────────────────────────────
  g_shGold = LoadShaderFromMemory(VS_GOLD, FS_GOLD);
  g_locGoldTime   = GetShaderLocation(g_shGold, "time");
  g_locGoldCamPos = GetShaderLocation(g_shGold, "cameraPos");
  g_locGoldSunDir = GetShaderLocation(g_shGold, "sunDir");
  g_shWater = LoadShaderFromMemory(nullptr, FS_WATER);
  g_locWaterTime = GetShaderLocation(g_shWater, "time");
  // Gold unit-cube model (used by DrawGoldBuilding)
  Mesh goldMesh = GenMeshCube(1.0f, 1.0f, 1.0f);
  g_goldModel   = LoadModelFromMesh(goldMesh);
  g_goldModel.materials[0].shader = g_shGold;
  // Water plane model (used by DrawWaterPlane)
  Mesh waterMesh = GenMeshPlane(1.0f, 1.0f, 1, 1);
  g_waterModel   = LoadModelFromMesh(waterMesh);
  g_waterModel.materials[0].shader = g_shWater;
  g_cityModelsReady = true;
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

enum NpcRole { NPC_ROLE_NORMAL=0, NPC_ROLE_SHOP, NPC_ROLE_TOURNAMENT };
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
  NpcRole role;         // NPC_ROLE_SHOP opens shop, NPC_ROLE_TOURNAMENT starts tournament
  int cityIndex;        // city (0-4) for tournament masters
  // ── Nomadic traveller state ──
  bool nomadic;         // true: moves between cities along the road network
  float destX, destZ;   // current destination world-space
  float waitTimer;      // time left waiting at current city (seconds)
  int   nomadicItem;    // ItemId this nomad can sell (-1 = none / already sold)
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

// Fast player-distance squared (ground plane only, no sqrt) — placed after g_player
static inline float DistSqToPlayer(float wx, float wz) {
  float dx = wx - g_player.posX, dz = wz - g_player.posZ;
  return dx*dx + dz*dz;
}
static Texture2D g_sandTex, g_stormTex, g_torchGlow, g_signTextures[4];
static std::vector<Tent> g_tents;
static std::vector<Rock> g_rocks;
static NPC g_npcs[60];
static int g_numNpcs = 0;
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
static constexpr int MAX_FIELD = 5;
static constexpr int MAX_DECK = 60;
static constexpr int MAX_GRAVE = 60;
static constexpr int MAX_OBLIVION = 30;

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
  int oblivion[MAX_OBLIVION]; // banished cards
  int oblivionSize;
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

// ── Match Visual Effects ────────────────────────────────────────────────────
static constexpr int MAX_FLOAT_TEXT = 16;
struct FloatText {
  char text[32];
  float x, y;
  float life;
  Color color;
  bool active;
};
static FloatText g_floatTexts[MAX_FLOAT_TEXT];
static int g_matchHoverHand = -1;
static int g_matchHoverField = -1;
static int g_matchHoverEnemyField = -1;
// ── Drag-drop / zoom state ────────────────────────────────────────────────────
static int    g_dragCardIdx   = -1;    // hand index being dragged (-1 = none)
static bool   g_dragActive    = false; // true once mouse moved > 20px from press
static Vector2 g_dragPos      = {};    // mouse position this frame
static Vector2 g_dragStartPos = {};    // mouse position at button press
static int    g_zoomedCard    = -1;    // hand index shown zoomed; -1 = none
static int    g_kbHandSel     = -1;    // keyboard-highlighted hand card

// ── Graveyard gallery modal ────────────────────────────────────────────────────
static bool   g_graveModalOpen   = false;
static int    g_graveModalPlayer = 0;   // 0=human, 1=AI
static float  g_graveScrollY     = 0.f; // vertical scroll offset in the modal
// Turn banner flag — slides in when turn changes, countdown timer
static float  g_turnBannerTimer  = 0.f;
static int    g_turnBannerWho    = 0;   // 0=player, 1=opponent (captured at trigger)

// Fixed 3D Perspective Camera at 60° downward tilt (spec: physical table feel)
// Position (0, 20, 11.5): atan2(20, 11.5) ≈ 60.1° depression angle
static Camera3D g_matchCam = {
    {0.0f, 20.0f, 11.5f},  // position: 60° downward tilt above table
    {0.0f, 0.0f, 0.0f},    // target: center of table
    {0.0f, 1.0f, 0.0f},    // up
    60.0f,                   // 60° perspective FOV (spec)
    CAMERA_PERSPECTIVE};

// ── Card texture constants (declared early for arena draw helpers) ────────────
static constexpr int CARD_TEX_W = 96;
static constexpr int CARD_TEX_H = 128;
static Texture2D g_cardTextures[200]; // one per card ID (up to 193 + extra)
static int g_numCardTextures = 0;

// ── Arena VFX State ──────────────────────────────────────────────────────────
struct ArenaBurst {
  float x, y;    // screen-space position
  float vx, vy;  // velocity
  float life;    // 1.0 → 0.0
  float size;
  Color col;
  bool active;
};
static constexpr int ARENA_BURST_MAX = 80;
static ArenaBurst    g_arenaBursts[ARENA_BURST_MAX];

// Combat targeting arrow (screen-space, drawn during ACTIVATE selection)
static bool    g_arrowActive = false;
static Vector2 g_arrowSrc   = {};
static Vector2 g_arrowDst   = {};

// AI "Thinking..." indicator
static float g_aiThinkTimer  = 0.0f;

// Oblivion portal pulse phase
static float g_oblivionPulse = 0.0f;

// ── Spawn arena burst particles ──────────────────────────────────────────────
static void SpawnArenaBurst(float x, float y, int count, Color col) {
  for (int i = 0; i < count; i++) {
    for (int k = 0; k < ARENA_BURST_MAX; k++) {
      if (!g_arenaBursts[k].active) {
        float angle = (float)(rand() % 628) * 0.01f;
        float speed = 40.0f + (float)(rand() % 90);
        g_arenaBursts[k] = {x, y,
                             cosf(angle) * speed,
                             sinf(angle) * speed - 20.0f,
                             1.0f, (float)(3 + rand() % 5), col, true};
        break;
      }
    }
  }
}

// ── Update arena VFX (called from UpdateMatch each frame) ──────────────────

// ══════════════════════════════════════════════════════════════════════════════
// Forward declarations for anim helpers (defined later)
static Vector3 GetFieldSlotPos(int playerIdx, int slotIdx, int totalUnits);
static Rectangle GetHandCardRect(int idx, int handSize);

// ── CardView — must be defined before DrawCardAnims ──────────────────────────
struct CardView {
  int  cardId    = 0;
  int  atk       = 0;
  int  def       = 0;
  int  cost      = 0;
  int  rarity    = 0;   // 0=common  1=rare  2=unique
  bool isPlayable = false;
  float hoverRotX = 0.f, hoverRotY = 0.f;
  float hoverVelX = 0.f, hoverVelY = 0.f;
  float scale    = 1.f;
  float scaleVel = 0.f;

  void Init(int id, bool doEnter = false) {
    if (id <= 0 || id >= NUM_ALL_CARDS) return;
    const CardDef &cd = ALL_CARDS[id];
    cardId = cd.id; atk = cd.atk; def = cd.def; cost = cd.cost;
    rarity = cd.isUnique ? 2 : cd.rarity;
    if (doEnter) { scale = 0.8f; scaleVel = 0.f; } else scale = 1.0f;
  }
  void Update(float dt, bool hovered, Vector2 mouseRel) {
    float tgtX = hovered ? Clamp( mouseRel.y * 0.018f, -0.25f, 0.25f) : 0.f;
    float tgtY = hovered ? Clamp(-mouseRel.x * 0.018f, -0.25f, 0.25f) : 0.f;
    const float k = 18.f, damp = 8.f;
    hoverVelX += (tgtX - hoverRotX) * k * dt - hoverVelX * damp * dt;
    hoverVelY += (tgtY - hoverRotY) * k * dt - hoverVelY * damp * dt;
    hoverRotX += hoverVelX * dt; hoverRotY += hoverVelY * dt;
    const float ks = 20.f, ds = 7.f;
    scaleVel += (1.0f - scale) * ks * dt - scaleVel * ds * dt;
    scale     = Clamp(scale + scaleVel * dt, 0.0f, 1.15f);
  }
  void UpdateStats(int newAtk, int newDef) { atk = newAtk; def = newDef; }
};

static void DrawCardView(Rectangle rect, const CardView &view, int artTexId); // fwd

// §CARD-ANIM   Bezier Tweening Engine (non-blocking card animations)
// ══════════════════════════════════════════════════════════════════════════════

// ── Easing functions ──────────────────────────────────────────────────────────
static float EaseInOutCubic(float t) {
  t = Clamp(t, 0.f, 1.f);
  return t < 0.5f ? 4*t*t*t : 1.f - powf(-2*t+2, 3)*0.5f;
}
static float EaseOutBack(float t) {          // overshoot for "slam"
  t = Clamp(t, 0.f, 1.f);
  const float c1 = 1.70158f, c3 = c1 + 1.f;
  return 1.f + c3*powf(t-1.f,3) + c1*powf(t-1.f,2);
}
static float EaseInCubic(float t)  { t=Clamp(t,0.f,1.f); return t*t*t; }
static float EaseOutCubic(float t) { t=Clamp(t,0.f,1.f); float s=1-t; return 1-s*s*s; }

// ── Bezier helpers ────────────────────────────────────────────────────────────
static Vector2 BezQ(Vector2 p0, Vector2 p1, Vector2 p2, float t) {
  float m = 1-t;
  return {m*m*p0.x + 2*m*t*p1.x + t*t*p2.x,
          m*m*p0.y + 2*m*t*p1.y + t*t*p2.y};
}

// ── Screen-shake ──────────────────────────────────────────────────────────────
static float g_shakeTimer = 0.f, g_shakeMag = 0.f;

// ── Desert SFX — procedurally generated, no external files ───────────────────
enum SfxId {
  SFX_CARD_PLAY = 0,  // papyrus thump when unit deployed
  SFX_CARD_ATTACK,    // sharp crack on attack lunge
  SFX_CARD_DESTROY,   // sandy crumble on destroy
  SFX_CARD_DRAW,      // airy whoosh on draw
  SFX_SHOCKWAVE,      // deep bass thud on card-play landing
  SFX_CONFIRM,        // bright desert chime — UI confirm
  SFX_FOOTSTEP,       // soft sand crunch — overworld step
  SFX_COUNT
};
static Sound g_sfx[SFX_COUNT] = {};
static bool  g_audioReady     = false;

static void PlaySfx(SfxId id) {
  if (g_audioReady && id >= 0 && id < SFX_COUNT) PlaySound(g_sfx[id]);
}

// waveform function type
typedef float (*WaveFn)(float t);

static Sound GenSound(WaveFn fn, float dur, int sr = 22050) {
  int n = (int)(dur * sr);
  Wave w = {(unsigned)n, (unsigned)sr, 16, 1, MemAlloc(n * 2)};
  short *s = (short *)w.data;
  for (int i = 0; i < n; i++) {
    float v = fn((float)i / sr);
    v = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
    s[i] = (short)(v * 32767.f);
  }
  Sound snd = LoadSoundFromWave(w);
  MemFree(w.data);
  return snd;
}

static float _sfx_play    (float t) { float e=expf(-t*16.f); return (sinf(t*1700.f)*0.45f+sinf(t*2400.f)*0.3f+sinf(t*900.f)*0.25f)*e*0.85f; }
static float _sfx_attack  (float t) { float e=expf(-t*32.f); return (sinf(t*4000.f)*0.55f+sinf(t*2200.f)*0.35f+sinf(t*350.f)*expf(-t*20.f)*0.4f)*e; }
static float _sfx_destroy (float t) { float e=expf(-t*5.5f); return (sinf(t*880.f+t*t*2800.f)*0.5f+sinf(t*660.f+t*t*1400.f)*0.5f)*e*0.9f; }
static float _sfx_draw    (float t) { float e=(t<0.10f)?t/0.10f:expf(-(t-0.10f)*18.f); return sinf(2.f*PI*(380.f+t*2100.f)*t)*e*0.55f; }
static float _sfx_shock   (float t) { float e=expf(-t*8.f); return (sinf(t*2.f*PI*(115.f-t*55.f))*0.8f+sinf(t*2.f*PI*750.f)*expf(-t*48.f)*0.2f)*e; }
static float _sfx_confirm (float t) { float e=expf(-t*7.f); return (sinf(t*2.f*PI*1760.f)*0.6f+sinf(t*2.f*PI*2640.f)*0.3f+sinf(t*2.f*PI*3520.f)*0.1f)*e; }
static float _sfx_step    (float t) { float e=expf(-t*50.f); return (sinf(t*1250.f)*0.5f+sinf(t*1950.f)*0.3f+sinf(t*2700.f)*0.2f)*e*0.42f; }

static void InitSounds() {
  InitAudioDevice();
  if (!IsAudioDeviceReady()) return;
  g_audioReady = true;
  g_sfx[SFX_CARD_PLAY]    = GenSound(_sfx_play,    0.28f);
  g_sfx[SFX_CARD_ATTACK]  = GenSound(_sfx_attack,  0.18f);
  g_sfx[SFX_CARD_DESTROY] = GenSound(_sfx_destroy, 0.55f);
  g_sfx[SFX_CARD_DRAW]    = GenSound(_sfx_draw,    0.25f);
  g_sfx[SFX_SHOCKWAVE]    = GenSound(_sfx_shock,   0.40f);
  g_sfx[SFX_CONFIRM]      = GenSound(_sfx_confirm, 0.35f);
  g_sfx[SFX_FOOTSTEP]     = GenSound(_sfx_step,    0.12f);
}
static void CloseSounds() {
  for (int i = 0; i < SFX_COUNT; i++) UnloadSound(g_sfx[i]);
  CloseAudioDevice();
}

// ── Table shockwave ring (expanding radial on card-play impact) ───────────────
struct ShockwaveState { bool active; Vector2 center; float elapsed, duration; };
static ShockwaveState g_shockwave = {};
static Vector2 ShakeOff() {
  if (g_shakeTimer <= 0.f) return {0,0};
  float t = g_shakeTimer;
  return {sinf(t * 89.3f) * g_shakeMag * t,
          cosf(t * 67.1f) * g_shakeMag * t};
}

// ── Animation pool ────────────────────────────────────────────────────────────
enum CardAnimType { CA_NONE=0, CA_DRAW, CA_PLAY, CA_DESTROY, CA_ERASE, CA_ATTACK };
struct CardAnim {
  bool          active;
  CardAnimType  type;
  int           cardId;
  float         elapsed, duration, startDelay;
  // Quadratic Bezier
  Vector2       p0, p1, p2;
  // Rebound (attack)
  Vector2       rb0, rb1, rb2;
  bool          rebounding;
  float         rbElapsed, rbDuration;
  // Visuals
  float         scale, alpha, rot;
  Color         glowColor;
  float         glowAlpha;
  bool          grayscale, showBack;
  float         dissolve;  // 0=solid 1=gone
  // Soul particle (destroy)
  bool          soulActive;
  Vector2       soulPos, soulDst;
  float         soulElapsed;
  // Entry flash flag (prevents re-triggering landing effects)
  bool          landedFx;
  // Draw-and-Reveal: player draw flips at t=0.5; AI draw stays face-down
  bool          isReveal;
  // Attack wind-up phase (pull-back before lunge)
  bool          windingUp;
  float         windupElapsed, windupDuration;
  Vector2       windupStart, windupPos;   // windupStart=origin, windupPos=pulled-back spot
  bool          impactFired;              // prevents double-firing impact FX
};
static constexpr int CA_POOL = 16;
static CardAnim g_cardAnims[CA_POOL];

// Enter-trigger display state
static float   g_enterFxTimer  = 0.f;
static Vector2 g_enterFxPos;
static Color   g_enterFxColor  = {255, 200, 60, 255};
static int     g_enterFxCardId = -1;

// Hand spring-spread offsets (X) driven by hover index
static float g_hspringOff[MAX_HAND] = {};
static float g_hspringVel[MAX_HAND] = {};

// ── Allocate ──────────────────────────────────────────────────────────────────
static CardAnim* AllocCA() {
  for (int i = 0; i < CA_POOL; i++) {
    if (!g_cardAnims[i].active) {
      g_cardAnims[i] = {};
      g_cardAnims[i].active = true;
      g_cardAnims[i].scale  = 1.f;
      g_cardAnims[i].alpha  = 1.f;
      return &g_cardAnims[i];
    }
  }
  return nullptr; // pool full
}

// ── AnimationQueue — centralized command sequencer ────────────────────────────
enum AnimCmdType { ACMD_NONE=0, ACMD_DRAW, ACMD_PLAY, ACMD_DESTROY, ACMD_ATTACK };
struct AnimCmd {
  AnimCmdType type;
  int         cardId;
  bool        parallelGroup; // fire alongside previous command without waiting
  bool        isPlayer;      // true=player (Draw-and-Reveal), false=AI (face-down)
  Vector2     from, ctrl, to;
  float       duration;
};
static constexpr int ANIM_Q_CAP = 32;
static AnimCmd g_animCmdQueue[ANIM_Q_CAP];
static int  g_animQHead     = 0;
static int  g_animQTail     = 0;
static bool g_animUILocked  = false;
static bool g_prevAnimLocked = false;   // for pending-draw flush edge detection

// Pending draw buffer: human player cards waiting for CA_DRAW to finish
static int  g_pendingDrawCards[32];
static int  g_pendingDrawCount = 0;

static bool AnyCA_Active() {
  for (int i = 0; i < CA_POOL; i++)
    if (g_cardAnims[i].active) return true;
  return false;
}

static void EnqueueAnim(AnimCmd cmd) {
  int next = (g_animQTail + 1) % ANIM_Q_CAP;
  if (next == g_animQHead) return; // queue full — drop
  g_animCmdQueue[g_animQTail] = cmd;
  g_animQTail = next;
}

static void FireAnimCmd(const AnimCmd &cmd) {
  CardAnim *a = AllocCA();
  if (!a) return;
  a->cardId    = cmd.cardId;
  a->duration  = cmd.duration > 0.f ? cmd.duration : 0.5f;
  a->p0 = cmd.from; a->p1 = cmd.ctrl; a->p2 = cmd.to;
  a->alpha = 1.f;
  switch (cmd.type) {
  case ACMD_DRAW:
    a->type     = CA_DRAW;
    a->showBack = true;
    a->isReveal = cmd.isPlayer;
    a->scale    = 0.75f;
    break;
  case ACMD_PLAY:
    a->type     = CA_PLAY;
    a->showBack = false;
    a->scale    = 1.f;
    a->duration = cmd.duration > 0.f ? cmd.duration : 0.4f;
    break;
  case ACMD_DESTROY:
    a->type    = CA_DESTROY;
    a->soulDst = {cmd.to.x, cmd.to.y};
    a->duration = cmd.duration > 0.f ? cmd.duration : 0.7f;
    break;
  case ACMD_ATTACK: {
    a->type      = CA_ATTACK;
    a->rb0 = cmd.to; a->rb1 = cmd.ctrl; a->rb2 = cmd.from;
    a->rbDuration   = 0.22f;
    a->duration     = cmd.duration > 0.f ? cmd.duration : 0.28f;
    // Wind-up: pull back 20px away from target direction
    a->windupStart    = cmd.from;
    a->windupDuration = 0.13f;
    a->windupElapsed  = 0.f;
    a->windingUp      = true;
    a->impactFired    = false;
    float dx = cmd.to.x - cmd.from.x, dy = cmd.to.y - cmd.from.y;
    float len = sqrtf(dx*dx + dy*dy);
    if (len > 0.001f)
      a->windupPos = {cmd.from.x - dx/len * 20.f, cmd.from.y - dy/len * 20.f};
    else
      a->windupPos = cmd.from;
    break;
  }
  default: a->active = false; break;
  }
}

// Called each frame from UpdateMatch/UpdateOverworld
static void UpdateAnimQueue() {
  if (g_animQHead == g_animQTail) {
    // Queue is empty — unlock UI if no active anims
    g_animUILocked = AnyCA_Active();
    return;
  }
  // Only advance queue when all current animations are done
  if (!AnyCA_Active()) {
    // Fire head command
    FireAnimCmd(g_animCmdQueue[g_animQHead]);
    g_animQHead = (g_animQHead + 1) % ANIM_Q_CAP;
    // Fire any consecutive commands in the same parallelGroup
    while (g_animQHead != g_animQTail &&
           g_animCmdQueue[g_animQHead].parallelGroup) {
      FireAnimCmd(g_animCmdQueue[g_animQHead]);
      g_animQHead = (g_animQHead + 1) % ANIM_Q_CAP;
    }
  }
  g_animUILocked = true; // queue not empty or anims active
}

// Flush the animation queue (e.g. on scene change)
static void ClearAnimQueue() {
  g_animQHead = g_animQTail = 0;
  for (int i = 0; i < CA_POOL; i++) g_cardAnims[i].active = false;
  g_animUILocked = false;
}

// ── Update ────────────────────────────────────────────────────────────────────
static void UpdateCardAnims(float dt) {
  g_shakeTimer = fmaxf(0.f, g_shakeTimer - dt);
  g_enterFxTimer = fmaxf(0.f, g_enterFxTimer - dt);
  if (g_shockwave.active) {
    g_shockwave.elapsed += dt;
    if (g_shockwave.elapsed >= g_shockwave.duration) g_shockwave.active = false;
  }

  for (int i = 0; i < CA_POOL; i++) {
    CardAnim &a = g_cardAnims[i];
    if (!a.active) continue;
    if (a.startDelay > 0.f) { a.startDelay -= dt; continue; }
    a.elapsed += dt;
    float t = Clamp(a.elapsed / a.duration, 0.f, 1.f);

    switch (a.type) {
    case CA_DRAW:
      if (a.isReveal) {
        // Player draw: face-down travel → flip at t=0.5 → reveal + settle 1.1x → back to 1.0x
        a.showBack = (t < 0.5f);
        if (t < 0.5f) {
          a.scale = 0.75f + t * 0.35f;  // grow during travel
        } else {
          float tp = (t - 0.5f) / 0.5f;          // 0..1 after flip
          float settle = (tp < 0.4f)
            ? (1.0f + 0.1f * sinf(tp * 3.14159f / 0.4f))  // 1.0→1.1→1.0
            : 1.0f;
          a.scale = settle;
        }
      } else {
        // AI draw: stays face-down the whole arc
        a.showBack = true;
        a.scale    = 0.75f + t * 0.25f;
      }
      a.alpha = 1.f;
      if (t >= 1.f) a.active = false;
      break;

    case CA_PLAY: {
      float peak = sinf(t * 3.14159f);
      a.scale     = 1.f + 0.22f * peak;
      a.glowAlpha = peak * 0.9f;
      a.alpha     = 1.f;
      if (!a.landedFx && t >= 0.88f) {
        a.landedFx = true;
        g_shakeTimer = 0.18f; g_shakeMag = 6.f;
        SpawnArenaBurst(a.p2.x, a.p2.y, 18, {220, 200, 80, 200});
        g_shockwave = {true, a.p2, 0.f, 0.48f};
        PlaySfx(SFX_SHOCKWAVE);
      }
      if (t >= 1.f) a.active = false;
      break;
    }
    case CA_DESTROY:
      if (t < 0.25f) {                    // shake phase
        a.rot  = sinf(t * 160.f) * 9.f;
        a.scale = 1.f;  a.alpha = 1.f;  a.grayscale = false;
      } else {                            // dissolve phase
        float d = (t - 0.25f) / 0.75f;
        a.rot       = 0.f;
        a.grayscale = true;
        a.dissolve  = d;
        a.alpha     = 1.f - d;
        a.scale     = 1.f - d * 0.3f;
        if (!a.soulActive && d > 0.28f) {
          a.soulActive  = true;
          a.soulPos     = a.p0;
          a.soulElapsed = 0.f;
        }
      }
      if (a.soulActive) {
        a.soulElapsed += dt;
        float st = Clamp(a.soulElapsed / 0.55f, 0.f, 1.f);
        a.soulPos = {a.p0.x + (a.soulDst.x - a.p0.x) * EaseInCubic(st),
                     a.p0.y + (a.soulDst.y - a.p0.y) * EaseInCubic(st)};
      }
      if (t >= 1.f) a.active = false;
      break;

    case CA_ERASE:
      a.scale = 1.f - EaseInCubic(t);
      a.rot   = t * 720.f;
      a.alpha = 1.f - t;
      if (t >= 1.f) a.active = false;
      break;

    case CA_ATTACK:
      if (a.windingUp) {
        // Phase 1: pull back (brief, snappy)
        a.windupElapsed += dt;
        float wt = Clamp(a.windupElapsed / a.windupDuration, 0.f, 1.f);
        a.scale = 1.f + wt * 0.05f;   // slight grow to show tension
        if (wt >= 1.f) {
          a.windingUp = false;
          a.elapsed   = 0.f;          // reset lunge timer
        }
      } else if (!a.rebounding) {
        // Phase 2: lunge toward target
        a.scale = 1.f + EaseOutCubic(t) * 0.22f;
        if (!a.impactFired && t >= 0.82f) {
          // Impact: shake + burst at target position
          a.impactFired   = true;
          g_shakeTimer    = 0.14f;  g_shakeMag = 7.f;
          SpawnArenaBurst(a.p2.x, a.p2.y, 12, {255, 180, 60, 200});
        }
        if (t >= 1.f) { a.rebounding = true; a.rbElapsed = 0.f; }
      } else {
        // Phase 3: snap back
        a.rbElapsed += dt;
        float rt = Clamp(a.rbElapsed / a.rbDuration, 0.f, 1.f);
        a.scale = 1.22f - EaseOutCubic(rt) * 0.22f;
        if (rt >= 1.f) a.active = false;
      }
      break;

    default: a.active = false; break;
    }
  }

  // Hand spring simulation
  GameMatch &m = g_match;
  MatchPlayer &human = m.players[0];
  int hov = (m.phase == PHASE_DEVELOP || m.phase == PHASE_ACTIVATE)
              ? g_matchHoverHand : -1;
  for (int i = 0; i < human.handSize && i < MAX_HAND; i++) {
    float tgt = 0.f;
    if (hov >= 0) {
      int d = i - hov;
      if      (d == -2) tgt = -9.f;
      else if (d == -1) tgt = -19.f;
      else if (d ==  1) tgt = +19.f;
      else if (d ==  2) tgt = +9.f;
    }
    float f = 14.f * (tgt - g_hspringOff[i]) - 7.f * g_hspringVel[i];
    g_hspringVel[i] += f * dt;
    g_hspringOff[i] += g_hspringVel[i] * dt;
  }
}

// ── Draw ──────────────────────────────────────────────────────────────────────
static void DrawCardAnims() {
  Vector2 shake = ShakeOff();
  float cw = 75.f, ch = 105.f;

  for (int i = 0; i < CA_POOL; i++) {
    CardAnim &a = g_cardAnims[i];
    if (!a.active || a.startDelay > 0.f) continue;
    float t = Clamp(a.elapsed / a.duration, 0.f, 1.f);

    // Compute screen position
    Vector2 pos;
    switch (a.type) {
    case CA_DRAW:    pos = BezQ(a.p0, a.p1, a.p2, EaseInOutCubic(t)); break;
    case CA_PLAY:    pos = BezQ(a.p0, a.p1, a.p2, EaseOutBack(Clamp(t,0,1))); break;
    case CA_DESTROY:
    case CA_ERASE:   pos = a.p0; break;
    case CA_ATTACK:
      if (a.windingUp) {
        // Lerp from origin toward pulled-back position
        float wt = Clamp(a.windupElapsed / a.windupDuration, 0.f, 1.f);
        float ewt = EaseOutCubic(wt);
        pos = {a.windupStart.x + (a.windupPos.x - a.windupStart.x) * ewt,
               a.windupStart.y + (a.windupPos.y - a.windupStart.y) * ewt};
      } else if (!a.rebounding) {
        // Lunge: Bezier from windupPos → target
        pos = BezQ(a.windupPos, a.p1, a.p2, EaseOutCubic(t));
      } else {
        float rt = Clamp(a.rbElapsed / a.rbDuration, 0.f, 1.f);
        pos = BezQ(a.rb0, a.rb1, a.rb2, EaseOutCubic(rt));
      }
      break;
    default: continue;
    }
    pos.x += shake.x;
    pos.y += shake.y;

    float w = cw * a.scale, h = ch * a.scale;
    Rectangle dr = {pos.x - w*0.5f, pos.y - h*0.5f, w, h};
    Vector2 origin = {w*0.5f, h*0.5f};

    // Full card frame — use DrawCardView for consistent layout
    if (!a.showBack && a.cardId > 0 && a.cardId < (int)NUM_ALL_CARDS) {
      CardView av; av.Init(a.cardId);
      av.scale = a.scale;
      DrawCardView(dr, av, a.cardId);
      // Dissolve / grayscale veil
      if (a.dissolve > 0.01f || a.grayscale) {
        unsigned char veilA = (unsigned char)Clamp(
            (a.dissolve * 200.f) + (a.grayscale ? 80.f : 0.f), 0.f, 220.f);
        DrawRectangleRounded(dr, 0.10f, 4, {0, 0, 0, veilA});
      }
    } else if (a.showBack) {
      DrawRectangleRounded(dr, 0.12f, 4, {48, 36, 68, (unsigned char)(a.alpha * 215)});
      DrawRectangleRoundedLinesEx(dr, 0.12f, 4, 2.f, {160, 130, 210, 180});
      DrawText("?", (int)(pos.x - 6), (int)(pos.y - 9), 20,
               {180, 150, 255, (unsigned char)(a.alpha * 255)});
    } else {
      DrawRectangleRounded(dr, 0.12f, 4, {40, 28, 12, 250});
      DrawRectangleRoundedLinesEx(dr, 0.12f, 4, 2.0f, {182, 150, 52, 230});
    }

    // Glow overlay (PLAY cost color) — drawn after card frame
    if (a.glowAlpha > 0.01f) {
      Color gc = a.glowColor; gc.a = (unsigned char)(a.glowAlpha * 110);
      DrawRectangleRounded(dr, 0.12f, 4, gc);
    }

    // Fade veil for alpha < 1 (draw / play animations)
    if (a.alpha < 0.99f) {
      DrawRectangleRounded(dr, 0.12f, 4, {0, 0, 0, (unsigned char)((1.f - a.alpha) * 210)});
    }

    // Soul particle (DESTROY)
    if (a.soulActive) {
      float sa = Clamp(1.f - a.soulElapsed / 0.55f, 0.f, 1.f);
      DrawCircle((int)a.soulPos.x, (int)a.soulPos.y,
                 (int)(7*sa + 2), {200, 180, 255, (unsigned char)(sa*190)});
      DrawCircle((int)a.soulPos.x, (int)a.soulPos.y,
                 (int)(3*sa + 1), {255, 245, 255, (unsigned char)(sa*255)});
    }
  }

  // Enter-trigger gold pulse border
  if (g_enterFxTimer > 0.f && g_enterFxCardId >= 0) {
    float alpha = fminf(g_enterFxTimer * 5.f, 1.f);
    float pulse = sinf(g_time * 14.f) * 0.5f + 0.5f;
    float ew = cw * (1.08f + pulse * 0.04f), eh = ch * (1.08f + pulse * 0.04f);
    Rectangle er = {g_enterFxPos.x - ew*0.5f, g_enterFxPos.y - eh*0.5f, ew, eh};
    DrawRectangleRoundedLinesEx(er, 0.12f, 4, 3.5f + pulse * 1.5f,
      {(unsigned char)(255*alpha), (unsigned char)((180 + (int)(pulse*75))*alpha),
       (unsigned char)(30*alpha),  (unsigned char)(230*alpha)});
    int tw = MeasureText("ENTER!", 11);
    DrawText("ENTER!", (int)(g_enterFxPos.x - tw/2), (int)(g_enterFxPos.y - eh*0.5f - 18),
             11, {255, 220, 80, (unsigned char)(200*alpha)});
  }

  // ── Table shockwave ring (card-play impact) ──────────────────────────────
  if (g_shockwave.active) {
    float sw_t  = g_shockwave.elapsed / g_shockwave.duration;
    float sw_r  = sw_t * 195.f;
    float sw_th = 9.f * (1.f - sw_t);           // ring thins as it expands
    unsigned char sw_a = (unsigned char)((1.f - sw_t) * (1.f - sw_t) * 160);
    if (sw_r > sw_th)
      DrawRing(g_shockwave.center, sw_r - sw_th, sw_r,
               0.f, 360.f, 40,
               {220, 200, 80, sw_a});
    // Inner bright core flash (only first quarter)
    if (sw_t < 0.25f) {
      float cf = 1.f - sw_t / 0.25f;
      DrawCircle((int)g_shockwave.center.x, (int)g_shockwave.center.y,
                 (int)(sw_r * 0.35f + 4),
                 {255, 240, 160, (unsigned char)(cf * 90)});
    }
  }
}

// ── Returns time (sec) until all queued+active animations finish ──────────────
static float AnimQueueEnd() {
  float maxEnd = 0.f;
  for (int i = 0; i < CA_POOL; i++) {
    if (!g_cardAnims[i].active) continue;
    float remaining = g_cardAnims[i].startDelay
                    + g_cardAnims[i].duration
                    - g_cardAnims[i].elapsed;
    if (remaining > maxEnd) maxEnd = remaining;
  }
  return fmaxf(0.f, maxEnd);
}

// ── Trigger helpers ───────────────────────────────────────────────────────────
static void AnimDraw(int cardId, int nextHandIdx, int handSize, float delay) {
  PlaySfx(SFX_CARD_DRAW);
  CardAnim *a = AllocCA(); if (!a) return;
  a->type      = CA_DRAW;
  a->cardId    = cardId;
  a->duration  = 0.50f;
  a->startDelay = AnimQueueEnd() + delay; // chain after current anim
  a->showBack  = true;
  a->isReveal  = true; // player draw always reveals
  // Start: player deck 3D projected
  Vector3 dk3 = {10.5f, 0.5f, 5.5f};
  a->p0 = GetWorldToScreen(dk3, g_matchCam);
  Rectangle hr = GetHandCardRect(nextHandIdx, handSize + 1);
  a->p2 = {hr.x + hr.width*0.5f, hr.y + hr.height*0.5f};
  a->p1 = {(a->p0.x + a->p2.x)*0.5f, fminf(a->p0.y, a->p2.y) - 195.f};
}

static void AnimPlay(int cardId, int handIdx, int handSize,
                     int fieldIdx, int newFieldSize, int playerIdx,
                     Color glowCol) {
  PlaySfx(SFX_CARD_PLAY);
  CardAnim *a = AllocCA(); if (!a) return;
  a->type      = CA_PLAY;
  a->cardId    = cardId;
  a->duration  = 0.50f;
  a->startDelay = AnimQueueEnd(); // chain after current anim
  a->glowColor = glowCol;
  if (playerIdx == 1) {
    // AI hand: mirror of player hand layout along top of screen
    const float cardW = 88.f, cardH = 126.f, gap = 6.f;
    float totalW = handSize * (cardW + gap) - gap;
    float startX = (SCREEN_W - totalW) * 0.5f;
    float center = (handSize - 1) * 0.5f;
    float dist   = fabsf((float)handIdx - center);
    float fanDip = dist * dist * 3.8f;
    float y = cardH + 10.f - fanDip; // mirrored: near top, edges dip upward
    a->p0 = {startX + handIdx * (cardW + gap) + cardW * 0.5f, y};
    Vector3 fp = GetFieldSlotPos(playerIdx, fieldIdx, newFieldSize);
    a->p2 = GetWorldToScreen(fp, g_matchCam);
    a->p1 = {(a->p0.x + a->p2.x)*0.5f, a->p0.y + 90.f}; // arc downward
  } else {
    Rectangle hr = GetHandCardRect(handIdx, handSize);
    a->p0 = {hr.x + hr.width*0.5f, hr.y + hr.height*0.5f};
    Vector3 fp = GetFieldSlotPos(playerIdx, fieldIdx, newFieldSize);
    a->p2 = GetWorldToScreen(fp, g_matchCam);
    a->p1 = {(a->p0.x + a->p2.x)*0.5f, a->p0.y - 90.f};
  }
}

static void AnimDestroy(int cardId, int playerIdx, int fieldIdx, int fieldSize) {
  PlaySfx(SFX_CARD_DESTROY);
  CardAnim *a = AllocCA(); if (!a) return;
  a->type       = CA_DESTROY;
  a->cardId     = cardId;
  a->duration   = 0.50f;
  a->startDelay = AnimQueueEnd();
  Vector3 fp  = GetFieldSlotPos(playerIdx, fieldIdx, fieldSize);
  a->p0       = GetWorldToScreen(fp, g_matchCam);
  // Soul target = graveyard
  Vector3 gv  = {-10.5f, 0.5f, (playerIdx == 0) ? 5.5f : -6.5f};
  a->soulDst  = GetWorldToScreen(gv, g_matchCam);
}

static void AnimErase(int cardId, int playerIdx, int fieldIdx, int fieldSize) {
  CardAnim *a = AllocCA(); if (!a) return;
  a->type       = CA_ERASE;
  a->cardId     = cardId;
  a->duration   = 0.50f;
  a->startDelay = AnimQueueEnd();
  Vector3 fp  = GetFieldSlotPos(playerIdx, fieldIdx, fieldSize);
  a->p0       = GetWorldToScreen(fp, g_matchCam);
}

static void AnimAttack(int atkPl, int atkIdx, int atkTotal,
                       int defPl, int defIdx, int defTotal) {
  PlaySfx(SFX_CARD_ATTACK);
  CardAnim *a = AllocCA(); if (!a) return;
  a->type       = CA_ATTACK;
  a->cardId     = g_match.players[atkPl].field[atkIdx].cardId;
  a->duration   = 0.25f;
  a->rbDuration = 0.25f;
  a->startDelay = AnimQueueEnd();
  Vector3 ap = GetFieldSlotPos(atkPl, atkIdx, atkTotal);
  a->p0 = GetWorldToScreen(ap, g_matchCam);
  Vector2 tgt;
  if (defIdx >= 0) {
    Vector3 dp = GetFieldSlotPos(defPl, defIdx, defTotal);
    tgt = GetWorldToScreen(dp, g_matchCam);
  } else {
    tgt = {SCREEN_W * 0.5f, (float)(defPl == 1 ? 62 : SCREEN_H - 175)};
  }
  // Lunge slightly past target
  Vector2 dir = {tgt.x - a->p0.x, tgt.y - a->p0.y};
  float len = sqrtf(dir.x*dir.x + dir.y*dir.y);
  if (len > 0.01f) { dir.x /= len; dir.y /= len; }
  a->p2 = {tgt.x + dir.x*22, tgt.y + dir.y*22};
  a->p1 = {(a->p0.x + a->p2.x)*0.5f, (a->p0.y + a->p2.y)*0.5f - 18.f};
  // Rebound back
  a->rb0 = a->p2;  a->rb2 = a->p0;
  a->rb1 = {(a->rb0.x + a->rb2.x)*0.5f, (a->rb0.y + a->rb2.y)*0.5f - 30.f};
}

static void UpdateArenaVFX(float dt) {
  for (int i = 0; i < ARENA_BURST_MAX; i++) {
    ArenaBurst &b = g_arenaBursts[i];
    if (!b.active) continue;
    b.x  += b.vx * dt;
    b.y  += b.vy * dt;
    b.vy += 120.0f * dt;  // gravity pull
    b.life -= dt * 1.6f;
    if (b.life <= 0) b.active = false;
  }
  if (g_aiThinkTimer > 0) g_aiThinkTimer -= dt;
  g_oblivionPulse += dt * 2.5f;
}

static void SpawnFloatText(float x, float y, const char *txt, Color col) {
  for (int i = 0; i < MAX_FLOAT_TEXT; i++) {
    if (!g_floatTexts[i].active) {
      snprintf(g_floatTexts[i].text, 32, "%s", txt);
      g_floatTexts[i].x = x;
      g_floatTexts[i].y = y;
      g_floatTexts[i].life = 1.5f;
      g_floatTexts[i].color = col;
      g_floatTexts[i].active = true;
      return;
    }
  }
}

static void UpdateFloatTexts(float dt) {
  for (int i = 0; i < MAX_FLOAT_TEXT; i++) {
    if (g_floatTexts[i].active) {
      g_floatTexts[i].y -= 40.0f * dt;
      g_floatTexts[i].life -= dt;
      if (g_floatTexts[i].life <= 0)
        g_floatTexts[i].active = false;
    }
  }
}

static void DrawFloatTexts() {
  for (int i = 0; i < MAX_FLOAT_TEXT; i++) {
    if (g_floatTexts[i].active) {
      unsigned char alpha =
          (unsigned char)(255 * fminf(g_floatTexts[i].life, 1.0f));
      Color c = g_floatTexts[i].color;
      c.a = alpha;
      DrawText(g_floatTexts[i].text, (int)g_floatTexts[i].x,
               (int)g_floatTexts[i].y, 22, c);
    }
  }
}

// Get 3D world position for a field card slot
static Vector3 GetFieldSlotPos(int playerIdx, int slotIdx, int totalUnits) {
  float z = (playerIdx == 0) ? 3.5f : -2.5f;  // player near camera = screen-bottom
  float spacing = 3.2f;
  float startX = -(totalUnits - 1) * spacing / 2.0f;
  return {startX + slotIdx * spacing, 0.08f, z};
}

// Get screen rectangle for a hand card (2D overlay)
static Rectangle GetHandCardRect(int idx, int handSize) {
  float cardW = 88, cardH = 126;
  float gap = 6;
  float totalW = handSize * (cardW + gap) - gap;
  float startX = (SCREEN_W - totalW) / 2.0f;
  // Fan arc: center card is highest, edge cards dip down parabolically
  float center = (handSize - 1) * 0.5f;
  float dist   = fabsf((float)idx - center);
  float fanDip = dist * dist * 3.8f;   // up to ~19px dip at edge of 7-card hand
  float y = SCREEN_H - cardH - 10 + fanDip;
  if (idx == g_matchHoverHand)
    y -= 40;                            // hover: lift card well above the zone
  return {startX + idx * (cardW + gap), y, cardW, cardH};
}

// Get screen rectangle for a field unit (projected from 3D)
static Rectangle GetFieldScreenRect(int playerIdx, int slotIdx, int totalUnits,
                                     float w, float h) {
  Vector3 pos = GetFieldSlotPos(playerIdx, slotIdx, totalUnits);
  Vector2 sp = GetWorldToScreen(pos, g_matchCam);
  return {sp.x - w / 2, sp.y - h / 2, w, h};
}

// ── Player Collection & Shop ────────────────────────────────────────────────
static constexpr int MAX_COLLECTION = 600;
static int g_collection[MAX_COLLECTION]; // card IDs owned
static int g_collectionSize = 0;
static int g_playerDeck[MAX_DECK]; // current deck (card IDs)
static int g_playerDeckSize = 0;

static const CardDef &GetCard(int id); // forward decl

// Count copies of a card already in the player's deck
static int CountCopiesInDeck(int cardId) {
  int count = 0;
  for (int i = 0; i < g_playerDeckSize; i++)
    if (g_playerDeck[i] == cardId) count++;
  return count;
}
// Check if a card can be added to deck (max 3 copies, unique = max 1)
static bool CanAddToDeck(int cardId) {
  if (g_playerDeckSize >= MAX_DECK) return false;
  const CardDef &cd = GetCard(cardId);
  int maxCopies = cd.isUnique ? 1 : 3;
  return CountCopiesInDeck(cardId) < maxCopies;
}

static int g_playerCoins = 100; // persistent coins for shop (GDD: start with 100)
static bool g_hasStarterDeck = false;
static int g_shopScroll = 0;
static int g_currentShopCity = -1;         // which city's shop we're in (0-4, -1=village)
static bool g_shopCityVisited[5] = {};     // first-visit free packs tracker per city

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
      if (currentPrice[i] > 1000.0f)
        currentPrice[i] = 1000.0f;
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

  void OnTournamentEnd(int league, const int *winDeckIds, int winCount,
                       const int *loseDeckIds, int loseCount) {
    // Per-rarity win multipliers by league (Bronze → Diamond)
    static const float winCommon[]  = {1.10f, 1.20f, 1.30f, 1.40f, 1.50f};
    static const float winRare[]    = {1.05f, 1.08f, 1.12f, 1.18f, 1.20f};
    static const float winEpic[]    = {1.01f, 1.01f, 1.02f, 1.06f, 1.08f};
    // Per-rarity loss multipliers by league
    static const float loseCommon[] = {1.00f, 0.99f, 0.95f, 0.90f, 0.80f};
    static const float loseRare[]   = {1.00f, 0.99f, 0.98f, 0.95f, 0.90f};
    static const float loseEpic[]   = {1.00f, 0.99f, 0.99f, 0.98f, 0.95f};
    int li = (league >= 0 && league < 5) ? league : 0;
    for (int i = 0; i < winCount; i++) {
      int cid = winDeckIds[i];
      if (cid < 1 || cid > NUM_ALL_CARDS) continue;
      CardRarity r = GetCardRarity(cid);
      if      (r == RARITY_RARE) volatility[cid] *= winRare[li];
      else if (r == RARITY_EPIC) volatility[cid] *= winEpic[li];
      else                       volatility[cid] *= winCommon[li];
    }
    for (int i = 0; i < loseCount; i++) {
      int cid = loseDeckIds[i];
      if (cid < 1 || cid > NUM_ALL_CARDS) continue;
      CardRarity r = GetCardRarity(cid);
      if      (r == RARITY_RARE) volatility[cid] *= loseRare[li];
      else if (r == RARITY_EPIC) volatility[cid] *= loseEpic[li];
      else                       volatility[cid] *= loseCommon[li];
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
    // Base sell: price * 0.9 (shop buys at -10%)
    // Seller's Certificate: sell at price * 1.1 (10% above price)
    float sellRate = (sellerCertMult > 1.0f) ? 1.1f : 0.9f;
    return (int)(currentPrice[cardId] * sellRate * dayMod + 0.5f);
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
    // Bronze=100, Silver=200, Gold=1000, Platinum=2000, Diamond=8000
    const int prizes[] = {100, 200, 1000, 2000, 8000};
    return prizes[currentLeague];
  }

  int GetEntryFee() {
    // Bronze=10, Silver=20, Gold=50, Platinum=100, Diamond=200
    const int fees[] = {10, 20, 50, 100, 200};
    return fees[currentLeague];
  }

  float GetDifficultyMult() {
    // Scale NPC deck strength: 1.0 (Bronze) to 2.5 (Diamond)
    return 1.0f + currentLeague * 0.375f;
  }

  bool AdvanceRound(bool won, int *winDeck, int winCount, int *loseDeck,
                    int loseCount) {
    if (won) {
      roundsWon++;
      g_playerCoins += GetPrizeCoins(); // sponsorship mult applied at call site
      // Apply tournament price changes (per-rarity, per-league)
      g_market.OnTournamentEnd(currentLeague, winDeck, winCount, loseDeck, loseCount);

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
static bool g_tournamentMode = false; // true = match is a tournament round

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
    cycleLength = 1080.0f; // 18 min (3 × 6 min: Morning / Evening / Night)
    current = TIME_MORNING;
  }

  void Update(float dt) {
    elapsed += dt;
    float phase = fmodf(elapsed, cycleLength) / cycleLength;
    if (phase < 0.333f)
      current = TIME_MORNING;
    else if (phase < 0.667f)
      current = TIME_EVENING;
    else
      current = TIME_NIGHT;
  }

  float GetShopMod() {
    switch (current) {
    case TIME_MORNING:
      return 1.03f; // +3% per GDD
    case TIME_DAY:
      return 1.00f;
    case TIME_EVENING:
      return 1.00f;
    case TIME_NIGHT:
      return 1.00f;
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

  // 0-1 position through the full cycle
  float GetPhaseNorm() const { return fmodf(elapsed, cycleLength) / cycleLength; }

  // Sky/background color matching the time of day
  Color GetSkyColor() const {
    float t = GetPhaseNorm();
    if (t < 0.333f) return {205, 172, 118, 255}; // morning: warm sand-gold
    if (t < 0.667f) return {218, 140, 75, 255};  // evening: desert sunset orange
    return {30, 38, 65, 255};                      // night: deep indigo
  }

  // Sun (day) or moon (night) screen-UV position {0-1, 0-1}
  // x arcs right-to-left during day; moon arcs left-to-right at night
  void GetCelestialUV(float *uvx, float *uvy) const {
    float t = GetPhaseNorm();
    if (t < 0.667f) {
      // Day arc: phase 0→0.667 maps to right→left (0.85→0.15)
      float d = t / 0.667f; // 0..1
      *uvx = 0.85f - d * 0.70f;
      *uvy = 0.28f - sinf(d * PI) * 0.18f; // peak at zenith, dip near horizons
    } else {
      // Night arc: phase 0.667→1.0 maps moon right-to-left, lower
      float d = (t - 0.667f) / 0.333f;
      *uvx = 0.18f + d * 0.64f;
      *uvy = 0.32f - sinf(d * PI) * 0.14f;
    }
  }

  bool IsMorning() const { return GetPhaseNorm() < 0.333f; }
  bool IsEvening() const { float p = GetPhaseNorm(); return p >= 0.333f && p < 0.667f; }
  bool IsNight() const { return GetPhaseNorm() >= 0.667f; }

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
  ITEM_BLESSED_AMULET,       // +50% coins from defeating NPCs
  ITEM_TRADERS_BADGE,
  ITEM_SWIFT_BOOTS,          // run in overworld
  ITEM_SPONSORSHIP_CONTRACT, // 2x tournament prize
  ITEM_SPECIAL_COUPON,       // -10% single pack price
  ITEM_SPECIAL_PROMOTION,    // -10% box price
  ITEM_MAGNIFYING_GLASS,     // -up to 20% single card price
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
    items[ITEM_BLESSED_AMULET] = {ITEM_BLESSED_AMULET, "Blessed Amulet",
                                   "+50% coins from defeating random NPCs.", false};
    items[ITEM_TRADERS_BADGE] = {ITEM_TRADERS_BADGE, "Trader's Badge",
                                 "Unlock rare cards in shops.", false};
    items[ITEM_SWIFT_BOOTS] = {ITEM_SWIFT_BOOTS, "Swift Boots",
                                "Press B to run in the overworld.", false};
    items[ITEM_SPONSORSHIP_CONTRACT] = {ITEM_SPONSORSHIP_CONTRACT, "Sponsorship Contract",
                                        "Receive double money from winning tournaments.", false};
    items[ITEM_SPECIAL_COUPON] = {ITEM_SPECIAL_COUPON, "Special Coupon",
                                   "Single packs cost 10% less.", false};
    items[ITEM_SPECIAL_PROMOTION] = {ITEM_SPECIAL_PROMOTION, "Special Promotion",
                                      "Boxes cost 10% less.", false};
    items[ITEM_MAGNIFYING_GLASS] = {ITEM_MAGNIFYING_GLASS, "Magnifying Glass",
                                     "Single cards cost up to 20% less.", false};
  }

  bool Has(ItemId id) { return items[id].owned; }
  void Give(ItemId id) { items[id].owned = true; }

  float GetSellMult()       { return Has(ITEM_SELLERS_CERT)          ? 1.2f  : 1.0f; }
  float GetWinCoinMult()    { return Has(ITEM_BLESSED_AMULET)         ? 1.5f  : 1.0f; }
  float GetTournamentMult() { return Has(ITEM_SPONSORSHIP_CONTRACT)   ? 2.0f  : 1.0f; }
  float GetPackMod()        { return Has(ITEM_SPECIAL_COUPON)         ? 0.9f  : 1.0f; }
  float GetBoxMod()         { return Has(ITEM_SPECIAL_PROMOTION)      ? 0.9f  : 1.0f; }
  float GetCardBuyMod()     { return Has(ITEM_MAGNIFYING_GLASS)
                                ? (1.0f - (float)(rand() % 20) / 100.f) : 1.0f; }
  bool HasFairScale()       { return Has(ITEM_FAIR_SCALE); }
  bool HasSwiftBoots()      { return Has(ITEM_SWIFT_BOOTS); }
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
  // Shop first-visit tracking
  bool shopCityVisited[5];
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
  for (int i = 0; i < 5; i++) sd.shopCityVisited[i] = g_shopCityVisited[i];

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
  for (int i = 0; i < 5; i++) g_shopCityVisited[i] = sd.shopCityVisited[i];
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
  memset(g_floatTexts, 0, sizeof(g_floatTexts));
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
    mp.oblivionSize = 0;
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
  if (&mp == &g_match.players[0]) {
    // Human player: fire animation FIRST, add card to hand only after anim ends
    if (mp.handSize >= MAX_HAND)
      return false; // hand full
    int cardId = mp.deck[--mp.deckSize];
    AnimDraw(cardId, mp.handSize, mp.handSize, 0.f);
    if (g_pendingDrawCount < 32)
      g_pendingDrawCards[g_pendingDrawCount++] = cardId;
    return true;
  }
  // AI player: no animation, add immediately
  if (mp.handSize >= MAX_HAND)
    return false;
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
  // Animate: card travels from hand to field
  {
    Color gc = {220, 180, 40, 255};  // yellow = coin cost
    AnimPlay(cardId, handIdx, mp.handSize, mp.fieldSize, mp.fieldSize + 1,
             playerIdx, gc);
  }
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

// ── Death trigger: fire effects when a unit dies ────────────────────────────
static void FireDeathTrigger(int cardId, MatchPlayer &owner, MatchPlayer &opp) {
  const CardDef &cd = GetCard(cardId);
  if (!strstr(cd.effect, "Death"))
    return;
  const char *eff = cd.effect;

  // "Death - Draw N card" pattern
  if (strstr(eff, "Death") && strstr(eff, "Draw")) {
    int n = 1;
    const char *p = strstr(eff, "Draw ");
    if (p) { p += 5; if (*p >= '1' && *p <= '9') n = *p - '0'; }
    for (int i = 0; i < n; i++) MatchDrawCard(owner);
  }
  // "Death - Gain N life"
  if (strstr(eff, "Death") && strstr(eff, "Gain") && strstr(eff, "life")) {
    int n = 3;
    const char *p = strstr(eff, "Gain ");
    if (p) { p += 5; if (*p >= '1' && *p <= '9') n = *p - '0'; }
    owner.life += n;
  }
  // "Death - Gain N coin"
  if (strstr(eff, "Death") && strstr(eff, "Gain") && strstr(eff, "coin")) {
    int n = 1;
    const char *p = strstr(eff, "Gain ");
    if (p) { p += 5; if (*p >= '1' && *p <= '9') n = *p - '0'; }
    owner.coins += n;
  }
  // "you lose N life"
  if (strstr(eff, "Death") && strstr(eff, "you lose") && strstr(eff, "life")) {
    int n = 3;
    const char *p = strstr(eff, "lose ");
    if (p) { p += 5; if (*p >= '1' && *p <= '9') n = *p - '0'; }
    owner.life -= n;
  }
  // "Death - Deal N damage"
  if (strstr(eff, "Death") && strstr(eff, "Deal") && strstr(eff, "damage")) {
    int n = 3;
    const char *p = strstr(eff, "Deal ");
    if (p) { p += 5; if (*p >= '1' && *p <= '9') n = *p - '0'; }
    if (opp.fieldSize > 0) {
      int t = rand() % opp.fieldSize;
      opp.field[t].curDef -= n;
      if (opp.field[t].curDef + opp.field[t].powerCounters -
              opp.field[t].weakCounters <= 0)
        opp.field[t].alive = false;
    } else {
      opp.life -= n;
    }
  }
  // "Death - Opponent discards N card"
  if (strstr(eff, "Death") && strstr(eff, "discards")) {
    int n = 1;
    const char *p = strstr(eff, "discards ");
    if (p) { p += 9; if (*p >= '1' && *p <= '9') n = *p - '0'; }
    for (int i = 0; i < n && opp.handSize > 0; i++)
      MatchDiscard(opp, rand() % opp.handSize);
  }
  // "Death - Add this card to hand"
  if (strstr(eff, "Death") && strstr(eff, "Add this card to hand")) {
    if (owner.handSize < MAX_HAND)
      owner.hand[owner.handSize++] = cardId;
  }
  // "Death - Add 2 demons from your graveyard to your hand"
  if (strstr(eff, "Death") && strstr(eff, "Add") && strstr(eff, "demons") &&
      strstr(eff, "graveyard")) {
    int added = 0;
    for (int i = owner.graveSize - 1; i >= 0 && added < 2; i--) {
      const CardDef &gc = GetCard(owner.grave[i]);
      if (gc.isUnit && gc.subtype && strstr(gc.subtype, "demon")) {
        if (owner.handSize < MAX_HAND) {
          owner.hand[owner.handSize++] = owner.grave[i];
          // Remove from grave
          for (int j = i; j < owner.graveSize - 1; j++)
            owner.grave[j] = owner.grave[j + 1];
          owner.graveSize--;
          added++;
        }
      }
    }
  }
  // "Death - Put N weak counters on each enemy"
  if (strstr(eff, "Death") && strstr(eff, "weak counter") && strstr(eff, "enemy")) {
    int n = 1;
    const char *p = strstr(eff, "Put ");
    if (p) { p += 4; if (*p >= '1' && *p <= '9') n = *p - '0'; }
    for (int i = 0; i < opp.fieldSize; i++)
      opp.field[i].weakCounters += n;
  }
  // "Death - Make a N/N golem token"
  if (strstr(eff, "Death") && strstr(eff, "Make a") && strstr(eff, "token")) {
    int n = 5;
    const char *p = strstr(eff, "Make a ");
    if (p) { p += 7; if (*p >= '1' && *p <= '9') n = *p - '0'; }
    if (owner.fieldSize < MAX_FIELD) {
      FieldUnit &tok = owner.field[owner.fieldSize++];
      tok = {};
      tok.cardId = cardId;
      tok.curAtk = n; tok.curDef = n;
      tok.alive = true; tok.canActivate = false;
    }
  }
  // "opponent loses N life" (without "you lose")
  if (strstr(eff, "Death") && strstr(eff, "opponent loses") && strstr(eff, "life")) {
    int n = 4;
    const char *p = strstr(eff, "opponent loses ");
    if (p) { p += 15; if (*p >= '1' && *p <= '9') n = *p - '0'; }
    opp.life -= n;
  }
  // "Death - Your opponent must choose to sacrifice a unit or discard 2 cards or lose 8 life"
  if (strstr(eff, "Death") && strstr(eff, "sacrifice a unit or discard")) {
    if (opp.fieldSize > 0)
      opp.field[rand() % opp.fieldSize].alive = false;
    else if (opp.handSize >= 2) {
      MatchDiscard(opp, rand() % opp.handSize);
      MatchDiscard(opp, rand() % opp.handSize);
    } else
      opp.life -= 8;
  }
}

// ── Remove dead units from field ────────────────────────────────────────────
static void CleanupField(MatchPlayer &mp, MatchPlayer *opp = nullptr) {
  int w = 0;
  for (int i = 0; i < mp.fieldSize; i++) {
    int effDef = mp.field[i].curDef + mp.field[i].powerCounters -
                 mp.field[i].weakCounters;
    if (!mp.field[i].alive || effDef <= 0) {
      // Fire death trigger before moving to graveyard
      if (opp) FireDeathTrigger(mp.field[i].cardId, mp, *opp);
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
      CleanupField(opp, &mp);
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
      CleanupField(opp, &mp);
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
      CleanupField(opp, &mp);
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
      CleanupField(opp, &mp);
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

  // Support card play animation — arc toward table centre (burst on landing)
  {
    Color gc = {80, 200, 255, 255}; // blue = discard/support cost
    AnimPlay(cardId, handIdx, mp.handSize, 0, 1, playerIdx, gc);
    // Redirect landing to table centre instead of field slot
    CardAnim *last = nullptr;
    for (int _i = 0; _i < CA_POOL; _i++)
      if (g_cardAnims[_i].active && g_cardAnims[_i].cardId == cardId)
        last = &g_cardAnims[_i];
    if (last) {
      // Centre of the table
      Vector3 tc = {0.f, 0.2f, 0.f};
      last->p2 = GetWorldToScreen(tc, g_matchCam);
      last->p1 = {(last->p0.x + last->p2.x)*0.5f,
                  last->p0.y + (playerIdx == 1 ? 110.f : -110.f)};
    }
  }

  // Remove card from hand to graveyard
  if (mp.graveSize < MAX_GRAVE)
    mp.grave[mp.graveSize++] = cardId;
  for (int i = handIdx; i < mp.handSize - 1; i++)
    mp.hand[i] = mp.hand[i + 1];
  mp.handSize--;

  CleanupField(mp, &opp);
  CleanupField(opp, &mp);
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
  // Attack lunge animation
  AnimAttack(atkPlayer, atkIdx, ap.fieldSize, defPlayer, defIdx,
             defIdx >= 0 ? dp.fieldSize : 0);

  if (defIdx < 0) {
    // Direct attack on player
    dp.life -= aAtk;
    attacker.activated = true;
    // Floating damage text
    {
      char dmgBuf[16];
      snprintf(dmgBuf, 16, "-%d", aAtk);
      float fx = (defPlayer == 1) ? SCREEN_W / 2.0f : SCREEN_W / 2.0f;
      float fy = (defPlayer == 1) ? 50.0f : SCREEN_H - 190.0f;
      SpawnFloatText(fx, fy, dmgBuf, {255, 60, 60, 255});
    }
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
    AnimDestroy(defender.cardId, defPlayer, defIdx, dp.fieldSize);
    SpawnFloatText(SCREEN_W / 2.0f + 40, SCREEN_H / 2.0f - 30,
                   "DESTROYED", {255, 80, 80, 255});
    if (atkOverrun) {
      dp.life -= (aAtk - dDef);
      char ob[16];
      snprintf(ob, 16, "-%d OVR", aAtk - dDef);
      SpawnFloatText(SCREEN_W / 2.0f, (defPlayer == 1) ? 50.0f : SCREEN_H - 190.0f,
                     ob, {255, 120, 60, 255});
    }
  } else if (aAtk == dDef) {
    // Tie: both destroyed unless Tenacity
    SpawnFloatText(SCREEN_W / 2.0f + 40, SCREEN_H / 2.0f - 30,
                   "TRADE", {255, 200, 80, 255});
    if (!defTenacity) {
      defender.alive = false;
      AnimDestroy(defender.cardId, defPlayer, defIdx, dp.fieldSize);
    } else
      SpawnFloatText(SCREEN_W / 2.0f + 40, SCREEN_H / 2.0f + 10,
                     "TENACITY!", {200, 255, 200, 255});
    if (!atkTenacity) {
      attacker.alive = false;
      AnimDestroy(attacker.cardId, atkPlayer, atkIdx, ap.fieldSize);
    } else
      SpawnFloatText(SCREEN_W / 2.0f - 60, SCREEN_H / 2.0f + 10,
                     "TENACITY!", {200, 255, 200, 255});
  } else {
    // ATK < DEF: attacker destroyed, defender DEF reduced until end of turn
    attacker.alive = false;
    AnimDestroy(attacker.cardId, atkPlayer, atkIdx, ap.fieldSize);
    SpawnFloatText(SCREEN_W / 2.0f - 60, SCREEN_H / 2.0f + 10,
                   "REPELLED!", {255, 80, 80, 255});
    defender.bonusDef -= aAtk;  // temporary — reset at turn end
    char db[16];
    snprintf(db, 16, "-%d DEF", aAtk);
    SpawnFloatText(SCREEN_W / 2.0f + 40, SCREEN_H / 2.0f - 20,
                   db, {255, 180, 80, 255});
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

  CleanupField(ap, &dp);
  CleanupField(dp, &ap);
}

// ─── Trigger Harvest effects (fires at Collect phase start) ──────────────────
static void TriggerHarvest(MatchPlayer &mp, GameMatch &m) {
  for (int i = 0; i < mp.fieldSize; i++) {
    const CardDef &cd = GetCard(mp.field[i].cardId);
    if (!strstr(cd.effect, "Harvest")) continue;
    if (strstr(cd.effect, "Draw")) MatchDrawCard(mp);
    if (strstr(cd.effect, "Gain") && strstr(cd.effect, "coin")) {
      const char *p = strstr(cd.effect, "Gain");
      if (p) mp.coins += atoi(p + 5);
    }
    if (strstr(cd.effect, "Gain") && strstr(cd.effect, "life")) {
      const char *p = strstr(cd.effect, "Gain");
      if (p) mp.life += atoi(p + 5);
    }
    if (strstr(cd.effect, "power counter")) {
      const char *p = strstr(cd.effect, "Put");
      if (p) mp.field[i].powerCounters += atoi(p + 4);
    }
  }
  (void)m;
}

// ─── Trigger Closure effects (fires at End phase) ────────────────────────────
static void TriggerClosure(MatchPlayer &mp, GameMatch &m) {
  MatchPlayer &opp = (&mp == &m.players[0]) ? m.players[1] : m.players[0];
  for (int i = 0; i < mp.fieldSize; i++) {
    if (!mp.field[i].alive) continue;
    const CardDef &cd = GetCard(mp.field[i].cardId);
    if (!strstr(cd.effect, "Closure")) continue;

    // "become a defender" (Stone Golem)
    if (strstr(cd.effect, "become a defender")) {
      mp.field[i].isDefender = true;
    }
    // "You lose N life" (Angry Pacifist, Infernal Broodlord)
    if (strstr(cd.effect, "lose") && strstr(cd.effect, "life")) {
      const char *p = strstr(cd.effect, "lose ");
      if (p) { p += 5; int n = atoi(p); if (n > 0) mp.life -= n; }
    }
    // "Discard a card" / "Discard 1 card" (Furious Avenger, Infernal Broodlord)
    if (strstr(cd.effect, "Discard") && strstr(cd.effect, "card")) {
      const char *p = strstr(cd.effect, "Discard ");
      int n = 1;
      if (p) { p += 8; if (*p >= '1' && *p <= '9') n = *p - '0'; }
      for (int d = 0; d < n && mp.handSize > 0; d++) {
        int idx = mp.handSize - 1; // discard last card
        if (mp.graveSize < MAX_GRAVE)
          mp.grave[mp.graveSize++] = mp.hand[idx];
        mp.handSize--;
      }
    }
    // "Draw cards until you have N cards in hand" (Fortune Merchant)
    if (strstr(cd.effect, "Draw cards until")) {
      const char *p = strstr(cd.effect, "have ");
      if (p) {
        int target = atoi(p + 5);
        while (mp.handSize < target) {
          if (!MatchDrawCard(mp)) break;
        }
      }
    }
    // "Draw 1 card" (Rat Commander: conditional on 3+ rats attacked)
    else if (strstr(cd.effect, "3 or more rats") && strstr(cd.effect, "draw")) {
      if (m.ratsAttackedThisTurn >= 3)
        MatchDrawCard(mp);
    }
    // Generic "Draw" (simple draw 1)
    else if (strstr(cd.effect, "Draw")) {
      MatchDrawCard(mp);
    }
    // "Gain N life"
    if (strstr(cd.effect, "Gain") && strstr(cd.effect, "life")) {
      const char *p = strstr(cd.effect, "Gain ");
      if (p) mp.life += atoi(p + 5);
    }
    // "Gain N coin"
    if (strstr(cd.effect, "Gain") && strstr(cd.effect, "coin")) {
      const char *p = strstr(cd.effect, "Gain ");
      if (p) mp.coins += atoi(p + 5);
    }
    // "power counter"
    if (strstr(cd.effect, "power counter")) {
      const char *p = strstr(cd.effect, "Put");
      if (p) mp.field[i].powerCounters += atoi(p + 4);
    }
    // "Heal N life"
    if (strstr(cd.effect, "Heal") && strstr(cd.effect, "life")) {
      const char *p = strstr(cd.effect, "Heal ");
      if (p) mp.life += atoi(p + 5);
    }
    // "Sacrifice another ally or lose N life" (Infernal Overlord)
    if (strstr(cd.effect, "Sacrifice another ally") && strstr(cd.effect, "lose")) {
      bool sacrificed = false;
      for (int s = 0; s < mp.fieldSize; s++) {
        if (s == i || !mp.field[s].alive) continue;
        mp.field[s].alive = false;
        if (mp.graveSize < MAX_GRAVE)
          mp.grave[mp.graveSize++] = mp.field[s].cardId;
        sacrificed = true;
        break;
      }
      if (!sacrificed) {
        const char *p = strstr(cd.effect, "lose ");
        if (p) { p += 5; mp.life -= atoi(p); }
      }
    }
  }
  (void)opp;
}

// ─── Reset temporary combat modifiers at end of every turn ───────────────────
static void ResetTempModifiers(GameMatch &m) {
  for (int p = 0; p < 2; p++)
    for (int i = 0; i < m.players[p].fieldSize; i++) {
      m.players[p].field[i].bonusDef = 0;  // DEF reductions expire
      m.players[p].field[i].bonusAtk = 0;
    }
}

// ─── Grudge trigger: scan graveyard for "Grudge" effects each phase ─────────
static void TriggerGrudge(MatchPlayer &mp, MatchPlayer &opp) {
  for (int i = 0; i < mp.graveSize; i++) {
    const CardDef &cd = GetCard(mp.grave[i]);
    if (!strstr(cd.effect, "Grudge")) continue;
    // Parse grudge effect (future cards — placeholder patterns)
    if (strstr(cd.effect, "deal") && strstr(cd.effect, "damage")) {
      int n = 1;
      const char *p = strstr(cd.effect, "deal ");
      if (p) { p += 5; if (*p >= '1' && *p <= '9') n = *p - '0'; }
      opp.life -= n;
    }
    if (strstr(cd.effect, "gain") && strstr(cd.effect, "life")) {
      int n = 1;
      const char *p = strstr(cd.effect, "gain ");
      if (p) { p += 5; if (*p >= '1' && *p <= '9') n = *p - '0'; }
      mp.life += n;
    }
    if (strstr(cd.effect, "gain") && strstr(cd.effect, "coin")) {
      mp.coins += 1;
    }
  }
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
  TriggerHarvest(ai, m);  // Harvest effects for AI
  TriggerGrudge(ai, human);  // Grudge effects from graveyard
  // AI draw animation (use AI deck position — top of screen)

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
      CleanupField(ai, &human);
      CleanupField(human, &ai);
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
      // Must attack a defender (fly defenders can be attacked by all)
      int weakestDef = -1, weakestVal = 99999;
      for (int d = 0; d < human.fieldSize; d++) {
        if (!human.field[d].isDefender) continue;
        // Non-fly can attack fly defenders (rule: fly cannot be attacked unless defending)
        int val = human.field[d].curDef + human.field[d].powerCounters -
                  human.field[d].weakCounters;
        if (val < weakestVal) { weakestVal = val; weakestDef = d; }
      }
      if (weakestDef >= 0)
        ResolveCombat(m, 1, a, 0, weakestDef);
    } else {
      // Attack weakest attackable unit or player
      int weakest = -1;
      int weakestVal2 = 99999;
      for (int d = 0; d < human.fieldSize; d++) {
        bool targetFly = CardHasKeyword(GetCard(human.field[d].cardId), "fly");
        // Non-fly cannot attack non-defending fly units
        if (targetFly && !hasFly && !human.field[d].isDefender) continue;
        int val = human.field[d].curDef + human.field[d].powerCounters -
                  human.field[d].weakCounters + human.field[d].bonusDef;
        if (val < weakestVal2) { weakestVal2 = val; weakest = d; }
      }
      if (weakest >= 0)
        ResolveCombat(m, 1, a, 0, weakest);
      else
        ResolveCombat(m, 1, a, 0, -1);  // no valid unit target, attack player
    }
    CleanupField(ai, &human);
    CleanupField(human, &ai);
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

  // Closure effects fire at end of AI turn
  TriggerClosure(ai, m);
  // Temporary DEF reductions expire
  ResetTempModifiers(m);
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
  UpdateFloatTexts(dt);
  UpdateArenaVFX(dt);
  UpdateCardAnims(dt);
  UpdateAnimQueue();
  if (g_turnBannerTimer > 0.f) g_turnBannerTimer -= dt;

  // ── Graveyard gallery: close on ESC / scroll on wheel ─────────────────────
  if (g_graveModalOpen) {
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_Q))
      g_graveModalOpen = false;
    float wheel = GetMouseWheelMove();
    g_graveScrollY -= wheel * 80.f;
    if (g_graveScrollY < 0.f) g_graveScrollY = 0.f;
    return; // swallow all other input while modal is up
  }
  // ── Graveyard 3D click: project position to screen, test mouse ────────────
  if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && !g_animUILocked) {
    GameMatch &gm2 = g_match;
    Vector2 mouse2 = GetMousePosition();
    // Player graveyard at world (-10.5, 0.5, 5.5)
    Vector2 gravScreenP = GetWorldToScreen({-10.5f, 0.5f, 5.5f}, g_matchCam);
    // AI graveyard at world (-10.5, 0.5, -6.5)
    Vector2 gravScreenAI = GetWorldToScreen({-10.5f, 0.5f, -6.5f}, g_matchCam);
    const float hitR = 38.f; // hit radius in screen pixels
    if (gm2.players[0].graveSize > 0 &&
        CheckCollisionPointCircle(mouse2, gravScreenP, hitR)) {
      g_graveModalOpen   = true;
      g_graveModalPlayer = 0;
      g_graveScrollY     = 0.f;
    } else if (gm2.players[1].graveSize > 0 &&
               CheckCollisionPointCircle(mouse2, gravScreenAI, hitR)) {
      g_graveModalOpen   = true;
      g_graveModalPlayer = 1;
      g_graveScrollY     = 0.f;
    }
  }

  // Flush pending draw cards the moment animations finish
  if (g_prevAnimLocked && !g_animUILocked) {
    MatchPlayer &hp = g_match.players[0];
    for (int i = 0; i < g_pendingDrawCount; i++) {
      if (hp.handSize < MAX_HAND)
        hp.hand[hp.handSize++] = g_pendingDrawCards[i];
    }
    g_pendingDrawCount = 0;
  }
  g_prevAnimLocked = g_animUILocked;
  GameMatch &m = g_match;
  if (!m.active && m.phase == PHASE_GAME_OVER) {
    if (m.messageTimer > 0)
      m.messageTimer -= dt;
    if (IsKeyPressed(KEY_ENTER) || IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
      if (m.playerWon) {
        int reward = (int)(10 * g_inventory.GetWinCoinMult() + 0.5f);
        g_playerCoins += reward;
        if (g_tournamentMode) {
          int winDeck[MAX_DECK], loseDeck[MAX_DECK];
          int winCount = m.players[0].deckSize, loseCount = m.players[1].deckSize;
          for (int i = 0; i < winCount; i++)
            winDeck[i] = m.players[0].deck[i];
          for (int i = 0; i < loseCount; i++)
            loseDeck[i] = m.players[1].deck[i];
          bool cityDone = g_tournament.AdvanceRound(true, winDeck, winCount, loseDeck, loseCount);
          (void)cityDone;
          // Sponsorship Contract: double the tournament prize
          float sponsorBonus = g_inventory.GetTournamentMult() - 1.0f;
          if (sponsorBonus > 0.f)
            g_playerCoins += (int)(g_tournament.GetPrizeCoins() * sponsorBonus + 0.5f);
        }
      } else {
        if (g_tournamentMode) {
          // Lost tournament round — prices shift, round resets to 0
          int winDeck[MAX_DECK], loseDeck[MAX_DECK];
          int winCount = m.players[1].deckSize, loseCount = m.players[0].deckSize;
          for (int i = 0; i < winCount; i++)
            winDeck[i] = m.players[1].deck[i];
          for (int i = 0; i < loseCount; i++)
            loseDeck[i] = m.players[0].deck[i];
          g_tournament.roundsWon = 0; // reset round on loss
          g_market.OnTournamentEnd(g_tournament.currentLeague, winDeck, winCount, loseDeck, loseCount);
        }
      }
      g_tournamentMode = false;
      ClearAnimQueue();
      g_scene = SCENE_OVERWORLD;
    }
    return;
  }
  if (m.messageTimer > 0)
    m.messageTimer -= dt;

  MatchPlayer &human = m.players[0];
  MatchPlayer &ai = m.players[1];

  if (m.turn == 0) { // Human turn
    MatchPhase phaseSnap = m.phase; // snapshot — prevents same-frame cascade
    if (phaseSnap == PHASE_COLLECT) {
      if (!MatchDrawCard(human)) {
        m.phase = PHASE_GAME_OVER;
        m.playerWon = false;
        m.active = false;
        return;
      }
      human.coins += 2;
      TriggerHarvest(human, m);  // Harvest effects fire on collect
      TriggerGrudge(human, m.players[1]);  // Grudge effects from graveyard
      m.phase = PHASE_DEVELOP;
      snprintf(m.message, 128, "Your turn - Development Phase (play cards)");
      m.messageTimer = 1.5f;
    }

    if (phaseSnap == PHASE_DEVELOP) {
      Vector2 mouse = GetMousePosition();
      if (g_animUILocked) goto skip_human_input;

      // ── Helper lambda: try to play card ci from hand ──────────────────────
      auto tryPlayCard = [&](int ci) {
        if (ci < 0 || ci >= human.handSize) return;
        const CardDef &cd = GetCard(human.hand[ci]);
        if (cd.isUnit) {
          if (MatchDeployUnit(m, 0, ci)) {
            CleanupField(human, &ai); CleanupField(ai, &human);
            snprintf(m.message, 128, "Deployed %s", cd.name); m.messageTimer = 1.0f;
          } else {
            snprintf(m.message, 128, "Can't deploy: need %d coins (have %d)", cd.cost, human.coins);
            m.messageTimer = 1.0f;
          }
        } else {
          if (human.coins >= cd.cost) {
            PlaySupportCard(m, 0, ci);
            snprintf(m.message, 128, "Played %s", cd.name); m.messageTimer = 1.0f;
          } else {
            snprintf(m.message, 128, "Need %d coins (have %d)", cd.cost, human.coins);
            m.messageTimer = 1.0f;
          }
        }
      };

      // ── Zoomed card: input while a card is being inspected ────────────────
      if (g_zoomedCard >= 0 && g_zoomedCard < human.handSize) {
        if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON) || IsKeyPressed(KEY_ESCAPE) ||
            IsKeyPressed(KEY_Q)) {
          g_zoomedCard = -1;
        } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_E)) {
          int zi = g_zoomedCard; g_zoomedCard = -1;
          tryPlayCard(zi);
        }
        goto skip_human_input;
      }

      // ── Keyboard selection: number keys 1-9 zoom card for inspection ──────
      for (int i = 0; i < human.handSize && i < 9; i++) {
        if (IsKeyPressed(KEY_ONE + i)) { g_kbHandSel = i; g_zoomedCard = i; break; }
      }
      if (IsKeyPressed(KEY_TAB) && human.handSize > 0) {
        g_kbHandSel = (g_kbHandSel + 1) % human.handSize;
        g_zoomedCard = g_kbHandSel;
      }

      // ── Drag start: mouse press on a hand card ────────────────────────────
      if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && g_dragCardIdx < 0) {
        for (int i = 0; i < human.handSize; i++) {
          if (CheckCollisionPointRec(mouse, GetHandCardRect(i, human.handSize))) {
            g_dragCardIdx = i; g_dragStartPos = mouse; g_dragPos = mouse;
            g_dragActive = false; break;
          }
        }
      }

      // ── Drag update: track mouse while button held ────────────────────────
      if (g_dragCardIdx >= 0 && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        g_dragPos = mouse;
        float dx = mouse.x - g_dragStartPos.x, dy = mouse.y - g_dragStartPos.y;
        if (!g_dragActive && dx*dx + dy*dy > 400.f) g_dragActive = true;
      }

      // ── Drag release: play if dropped on field, zoom if just clicked ───────
      if (g_dragCardIdx >= 0 && IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
        int ci = g_dragCardIdx; g_dragCardIdx = -1;
        bool inField = (mouse.y < (float)(SCREEN_H - 150));
        if (g_dragActive && inField) {
          tryPlayCard(ci);
        } else if (!g_dragActive) {
          // Click without drag → zoom/inspect card
          g_zoomedCard = ci; g_kbHandSel = ci;
        }
        g_dragActive = false;
      }

      // ── Next Phase button or Space ────────────────────────────────────────
      Rectangle nextPhaseBtn = {(float)(SCREEN_W - 187), (float)(SCREEN_H - 185), 177, 30};
      bool advanceToActivate = IsKeyPressed(KEY_SPACE) ||
          (CheckCollisionPointRec(mouse, nextPhaseBtn) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON));
      if (advanceToActivate) {
        g_dragCardIdx = -1; g_dragActive = false; g_zoomedCard = -1;
        m.phase = PHASE_ACTIVATE; m.selectedFieldIdx = -1;
        snprintf(m.message, 128, "Activation Phase - drag to attack, right-click to defend");
        m.messageTimer = 1.5f;
      }
    }
    skip_human_input:;

    if (phaseSnap == PHASE_ACTIVATE && !g_animUILocked) {
      Vector2 mouse = GetMousePosition();

      // Select own unit to attack with
      if (m.selectedFieldIdx < 0) {
        for (int i = 0; i < human.fieldSize; i++) {
          Rectangle unitRect =
              GetFieldScreenRect(0, i, human.fieldSize, 65, 50);
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
          Rectangle unitRect =
              GetFieldScreenRect(1, i, ai.fieldSize, 65, 50);
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

        // Attack player button (positioned to match HUD)
        Rectangle atkPlayerBtn = {(float)SCREEN_W - 185, SCREEN_H - 213.0f,
                                   175, 30};
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

      // Right-click on own unit (no attacker selected) = toggle defender
      if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON) && m.selectedFieldIdx < 0) {
        for (int i = 0; i < human.fieldSize; i++) {
          Rectangle unitRect = GetFieldScreenRect(0, i, human.fieldSize, 65, 50);
          if (CheckCollisionPointRec(mouse, unitRect)) {
            const CardDef &cd = GetCard(human.field[i].cardId);
            if (!strstr(cd.effect, "Cannot defend."))
              human.field[i].isDefender = !human.field[i].isDefender;
            break;
          }
        }
      }
      // D key also toggles defender (keep for compatibility)
      if (IsKeyPressed(KEY_D)) {
        for (int i = 0; i < human.fieldSize; i++) {
          Rectangle unitRect = GetFieldScreenRect(0, i, human.fieldSize, 65, 50);
          if (CheckCollisionPointRec(mouse, unitRect)) {
            const CardDef &cd = GetCard(human.field[i].cardId);
            if (!strstr(cd.effect, "Cannot defend."))
              human.field[i].isDefender = !human.field[i].isDefender;
            break;
          }
        }
      }

      // End Turn button or Space/Enter
      Rectangle endTurnBtn = {(float)(SCREEN_W - 187), (float)(SCREEN_H - 185), 177, 30};
      bool doEndTurn = IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER) ||
          (CheckCollisionPointRec(mouse, endTurnBtn) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON));
      if (doEndTurn) {
        m.phase = PHASE_END;
      }
    }

    if (phaseSnap == PHASE_END) {
      // Closure effects fire at end of player turn
      TriggerClosure(human, m);
      // Temporary DEF reductions (from this combat turn) expire
      ResetTempModifiers(m);
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
      g_turnBannerTimer = 3.2f; g_turnBannerWho = 1; // opponent's turn banner
      g_aiThinkTimer = 1.4f;  // show Thinking... indicator
      AITakeTurn(m);
      g_aiThinkTimer = 0.0f;

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
      g_turnBannerTimer = 3.2f; g_turnBannerWho = 0; // your turn banner
    }
  }
}

// Forward declaration (DrawVFX defined later in overworld section)
static void DrawVFX();

// ═══════════════════════════════════════════════════════════════════════════════
// ARENA VISUAL HELPERS
// ═══════════════════════════════════════════════════════════════════════════════

// ── 3D Arena Torches + Chandelier ────────────────────────────────────────────
static void DrawArenaTorches() {
  // Torch positions: 4 corners alongside the table
  static const float TX[4] = {-13.5f,  13.5f, -13.5f, 13.5f};
  static const float TZ[4] = {  9.0f,   9.0f,  -7.5f, -7.5f};

  for (int t = 0; t < 4; t++) {
    float x = TX[t], z = TZ[t];
    float flk = sinf(g_time * (5.8f + t * 0.9f) + t * 2.3f) * 0.5f + 0.5f;

    // Stone base
    DrawCylinder({x, -0.10f, z}, 0.38f, 0.32f, 0.50f, 8, {100, 88, 70, 255});
    // Iron shaft
    DrawCylinder({x, 0.40f + 1.8f, z}, 0.065f, 0.065f, 3.60f, 6, {62, 56, 50, 255});
    // Torch head bracket
    DrawCylinder({x, 4.05f, z}, 0.15f, 0.12f, 0.28f, 8, {80, 72, 58, 255});

    // Flame (animated flicker)
    float fr = 0.22f + flk * 0.09f;
    DrawSphere({x, 4.28f + flk * 0.06f, z}, fr, {255, (unsigned char)(155 + flk * 85), 18, (unsigned char)(215 + flk * 35)});
    // Outer halo
    DrawSphere({x, 4.28f, z}, fr + 0.22f, {255, 115, 8, (unsigned char)(45 + flk * 40)});

    // Light pool on felt surface
    float pr = 2.2f + flk * 0.6f;
    DrawCylinder({x, -0.145f, z}, pr, pr, 0.008f, 14, {255, 148, 45, (unsigned char)(32 + flk * 22)});
  }

  // ── Overhead chandelier ────────────────────────────────────────────────────
  float chandY = 7.2f;
  float cflk = sinf(g_time * 4.5f) * 0.5f + 0.5f;

  // Suspension chains (4 anchor points high up to ring)
  for (int i = 0; i < 4; i++) {
    float a = i * (PI * 0.5f);
    DrawCylinder({cosf(a) * 0.8f, chandY + 0.4f, sinf(a) * 0.5f},
                 0.020f, 0.020f, 0.8f, 4, {140, 120, 65, 160});
  }
  // Brass ring
  for (int i = 0; i < 12; i++) {
    float a = i * (PI * 2.0f / 12.0f);
    DrawSphere({cosf(a) * 2.0f, chandY, sinf(a) * 1.2f}, 0.10f, {195, 168, 82, 200});
  }
  // Central hub
  DrawCylinder({0, chandY - 0.15f, 0}, 0.28f, 0.28f, 0.40f, 10, {175, 148, 65, 255});
  DrawSphere({0, chandY + 0.22f, 0}, 0.22f, {200, 172, 72, 255});

  // Candle flames on ring
  for (int i = 0; i < 6; i++) {
    float a = i * (PI * 2.0f / 6.0f);
    float cf = sinf(g_time * (6.2f + i * 0.6f) + i * 1.8f) * 0.5f + 0.5f;
    DrawSphere({cosf(a) * 2.0f, chandY - 0.62f + cf * 0.05f, sinf(a) * 1.2f},
               0.11f + cf * 0.04f,
               {255, (unsigned char)(165 + cf * 75), 25, (unsigned char)(200 + cf * 40)});
  }
  // Central glow
  DrawSphere({0, chandY - 0.38f, 0}, 0.16f + cflk * 0.06f,
             {255, (unsigned char)(195 + cflk * 55), 55, (unsigned char)(195 + cflk * 55)});
  // Chandelier floor pool (on table center)
  float fp = 4.8f + cflk * 0.5f;
  DrawCylinder({0, -0.145f, 0}, fp, fp, 0.005f, 20, {255, 175, 70, (unsigned char)(16 + cflk * 12)});
}

// Desert arena sky gradient background (drawn as 2D before 3D scene)
static void DrawArenaBackground() {
  // Deep warm dark background
  DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H,
                         {12, 8, 4, 255}, {32, 20, 10, 255});

  // Stone column silhouettes (left and right flanks)
  int colW = SCREEN_W / 12;
  DrawRectangleGradientH(0, 0, colW, SCREEN_H,
                         {6, 4, 2, 255}, {0, 0, 0, 0});
  DrawRectangleGradientH(SCREEN_W - colW, 0, colW, SCREEN_H,
                         {0, 0, 0, 0}, {6, 4, 2, 255});

  // Wall sconce glows (left and right, mid-height)
  float scY = (float)(SCREEN_H / 3);
  float scFlk1 = sinf(g_time * 6.1f) * 0.5f + 0.5f;
  float scFlk2 = sinf(g_time * 5.7f + 1.2f) * 0.5f + 0.5f;
  // Left sconce
  DrawCircleGradient(colW / 2 + 2, (int)scY,
                     colW * (1.8f + scFlk1 * 0.4f), {255, 155, 45, (unsigned char)(55 + scFlk1 * 35)}, {0, 0, 0, 0});
  DrawCircle(colW / 2 + 2, (int)scY, 6 + scFlk1 * 3, {255, 210, 110, (unsigned char)(200 + scFlk1 * 50)});
  // Right sconce
  DrawCircleGradient(SCREEN_W - colW / 2 - 2, (int)scY,
                     colW * (1.8f + scFlk2 * 0.4f), {255, 155, 45, (unsigned char)(55 + scFlk2 * 35)}, {0, 0, 0, 0});
  DrawCircle(SCREEN_W - colW / 2 - 2, (int)scY, 6 + scFlk2 * 3, {255, 210, 110, (unsigned char)(200 + scFlk2 * 50)});

  // Amber horizon haze band
  DrawRectangleGradientV(0, SCREEN_H / 2 - 50, SCREEN_W, 80,
                         {0, 0, 0, 0}, {190, 105, 40, 18});
  // Distant dune silhouette (procedural wave)
  for (int x = 0; x < SCREEN_W; x += 2) {
    int h = (int)(16.0f + sinf(x * 0.017f) * 9.0f + sinf(x * 0.044f + 1.1f) * 5.0f);
    DrawRectangle(x, SCREEN_H / 2 - h, 2, h, {45, 28, 10, 50});
  }
  // Warm overhead glow (chandelier bloom on ceiling)
  DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H / 4, {110, 75, 20, 22}, {0, 0, 0, 0});
}

// Draw a card texture flat on the 3D table surface using rlgl quads.
// Must be called inside BeginMode3D / EndMode3D.
static void DrawCardFaceTexture(Vector3 pos, float hw, float hd,
                                 int cardId, Color tint) {
  if (cardId <= 0 || cardId >= 200) return;
  Texture2D &tex = g_cardTextures[cardId];
  if (tex.id == 0) return;
  float y = pos.y + 0.09f;
  rlSetTexture(tex.id);
  rlBegin(RL_QUADS);
    rlColor4ub(tint.r, tint.g, tint.b, tint.a);
    rlTexCoord2f(0.0f, 0.0f); rlVertex3f(pos.x - hw, y, pos.z - hd);
    rlTexCoord2f(0.0f, 1.0f); rlVertex3f(pos.x - hw, y, pos.z + hd);
    rlTexCoord2f(1.0f, 1.0f); rlVertex3f(pos.x + hw, y, pos.z + hd);
    rlTexCoord2f(1.0f, 0.0f); rlVertex3f(pos.x + hw, y, pos.z - hd);
  rlEnd();
  rlSetTexture(0);
}

// Draw 5 recessed slot depressions per side (always 5, to show empty slots)
static void DrawSlotDepressions(int playerIdx, int numOccupied) {
  const float spacing = 3.2f;
  const float z = (playerIdx == 0) ? 3.5f : -2.5f;  // player near camera = bottom
  const float startX = -(5 - 1) * spacing / 2.0f;
  for (int i = 0; i < 5; i++) {
    float cx = startX + i * spacing;
    // Recessed felt pad (slightly lower, darker)
    DrawCube({cx, -0.17f, z}, 2.35f, 0.04f, 3.15f, {10, 34, 18, 200});
    // Slot outline — gold glow for occupied, faint for empty
    Color bc = (i < numOccupied)
                   ? Color{100, 85, 28, 110}
                   : Color{50, 42, 14, 45};
    DrawCubeWires({cx, -0.17f, z}, 2.40f, 0.05f, 3.20f, bc);
  }
}

// Draw a 3D unit card with its texture on the top face + keyword VFX.
// hasFly: hover higher + blob shadow
// hasDash: orange aura double-ring
// highlight: pulsing gold selection ring
static void DrawCard3DUnit(Vector3 pos, float cw, float dep, float thick,
                            int cardId, bool activated, bool highlight,
                            bool hasFly, bool hasDash, Color frameCol) {
  // Fly: card hovers 0.45 units higher
  float yOff = hasFly ? 0.45f : 0.0f;
  Vector3 rpos = {pos.x, pos.y + yOff, pos.z};

  if (activated) {
    // Tapped: rotate 90° around Y axis
    rlPushMatrix();
    rlTranslatef(rpos.x, rpos.y, rpos.z);
    rlRotatef(90.0f, 0, 1, 0);
    DrawCube({0, 0, 0}, dep, thick, cw, {45, 30, 18, 255});
    DrawCubeWires({0, 0, 0}, dep, thick, cw, frameCol);
    rlPopMatrix();
  } else {
    // Card slab body + art on top face
    DrawCube(rpos, cw, thick, dep, {45, 30, 18, 255});
    DrawCubeWires(rpos, cw, thick, dep, frameCol);
    DrawCardFaceTexture(rpos, cw * 0.5f, dep * 0.5f, cardId, WHITE);
  }

  // Dash keyword: double aura ring (orange spark)
  if (hasDash && !activated) {
    float pulse = sinf(g_time * 8.0f) * 0.5f + 0.5f;
    DrawCubeWires(rpos, cw + 0.18f, thick + 0.06f, dep + 0.18f,
                  {255, 160, 40, (unsigned char)(80 + pulse * 100)});
    DrawCubeWires(rpos, cw + 0.36f, thick + 0.10f, dep + 0.36f,
                  {255, 210, 80, (unsigned char)(25 + pulse * 45)});
  }

  // Fly keyword: blob shadow below the floating card
  if (hasFly) {
    float shadowScale = 0.72f + sinf(g_time * 1.4f) * 0.04f;
    DrawCube({pos.x, -0.14f, pos.z},
             cw * shadowScale, 0.01f, dep * shadowScale, {0, 0, 0, 55});
  }

  // Selection highlight: pulsing gold ring
  if (highlight) {
    float glow = sinf(g_time * 5.5f) * 0.5f + 0.5f;
    DrawCubeWires(rpos, cw + 0.28f, thick + 0.12f, dep + 0.28f,
                  {255, 225, 50, (unsigned char)(130 + glow * 125)});
  }
}

// Oblivion exile portal: concentric glowing rings that pulse
static void DrawOblivionPortal3D(Vector3 pos) {
  float pulse  = sinf(g_oblivionPulse) * 0.5f + 0.5f;
  float pulse2 = cosf(g_oblivionPulse * 0.7f) * 0.5f + 0.5f;
  DrawCylinder(pos, 0.40f + pulse * 0.12f, 0.65f + pulse * 0.18f,
               0.06f, 16, {70, 15, 110, (unsigned char)(150 + pulse * 80)});
  DrawCylinder({pos.x, pos.y + 0.03f, pos.z},
               0.65f + pulse * 0.18f, 0.95f + pulse2 * 0.22f,
               0.04f, 16, {120, 35, 175, (unsigned char)(55 + pulse2 * 55)});
  // Inner void
  DrawCylinder({pos.x, pos.y + 0.05f, pos.z},
               0.0f, 0.38f, 0.04f, 16, {8, 4, 18, 230});
}

// Screen-space quadratic Bezier targeting arrow (Hearthstone style)
static void DrawCombatArrow() {
  if (!g_arrowActive) return;
  Vector2 src  = g_arrowSrc;
  Vector2 dst  = g_arrowDst;
  Vector2 ctrl = {(src.x + dst.x) * 0.5f,
                  fminf(src.y, dst.y) - 55.0f};
  const int SEGS = 22;
  Vector2 prev = src;
  for (int i = 1; i <= SEGS; i++) {
    float t = (float)i / SEGS;
    float u = 1.0f - t;
    Vector2 pt = {
      u * u * src.x + 2 * u * t * ctrl.x + t * t * dst.x,
      u * u * src.y + 2 * u * t * ctrl.y + t * t * dst.y
    };
    float alpha = 0.5f + 0.5f * t;
    DrawLineEx(prev, pt, 3.2f, {255, 70, 60, (unsigned char)(alpha * 215)});
    prev = pt;
  }
  // Arrow head
  Vector2 dir  = Vector2Normalize(Vector2Subtract(dst, ctrl));
  Vector2 perp = {-dir.y, dir.x};
  DrawTriangle(
    {dst.x + dir.x * 13,  dst.y + dir.y * 13},
    {dst.x - dir.x * 6 + perp.x * 8, dst.y - dir.y * 6 + perp.y * 8},
    {dst.x - dir.x * 6 - perp.x * 8, dst.y - dir.y * 6 - perp.y * 8},
    {255, 80, 80, 220}
  );
}

// Visual phase bar: 4 segments (COLLECT→DEVELOP→ACTIVATE→END), active glows
// ── Per-phase waving flag pennant (drawn above each phase segment) ────────────
static void DrawPhaseFlagIcon(float px, float py, Color col, bool active) {
  // Pole
  Color poleCol = active ? Color{220, 210, 170, 255} : Color{110, 100, 75, 160};
  DrawLine((int)px, (int)(py - 2), (int)px, (int)(py - 20), poleCol);
  // Pennant — triangle pointing right, waves when active
  float wave = active ? sinf(g_time * 6.0f) * 2.8f : 0.0f;
  Color fc = col;
  fc.a     = active ? 230 : 90;
  // filled triangle: tip at pole top, tail at bottom, point to the right with wave
  Vector2 a2 = {px,       py - 20};
  Vector2 b2 = {px,       py - 10};
  Vector2 c2 = {px + 18,  py - 15 + wave};
  DrawTriangle(a2, b2, c2, fc);
  // bright outline rim when active
  if (active) {
    Color rim = {(unsigned char)fminf(col.r + 60, 255),
                 (unsigned char)fminf(col.g + 60, 255),
                 (unsigned char)fminf(col.b + 60, 255), 200};
    DrawTriangleLines(a2, b2, c2, rim);
  }
}

static void DrawPhaseBar(MatchPhase phase, bool isPlayerTurn) {
  const char *labels[]  = {"COLLECT", "DEVELOP", "ACTIVATE", "END"};
  const Color phaseColors[] = {
    {55, 175, 215, 255},   // COLLECT  – blue
    {70, 175,  70, 255},   // DEVELOP  – green
    {215,  75,  75, 255},  // ACTIVATE – red
    {165, 140,  65, 255},  // END      – gold
  };
  int numPhases   = 4;
  int activePhase = ((int)phase < numPhases) ? (int)phase : numPhases - 1;
  float barW  = 290.0f;
  float barH  = 22.0f;
  float startX = SCREEN_W / 2.0f - barW / 2.0f;
  float y      = 28.0f;  // shifted down to make room for flags above
  float segW   = barW / numPhases;

  DrawRectangleRounded({startX - 4, y - 2, barW + 8, barH + 4},
                        0.4f, 4, {14, 10, 7, 205});

  for (int i = 0; i < numPhases; i++) {
    float sx    = startX + i * segW;
    bool  active = (i == activePhase && isPlayerTurn);
    float pulse  = active ? (sinf(g_time * 4.0f) * 0.3f + 0.7f) : 0.32f;
    Color col    = phaseColors[i];
    col.a = (unsigned char)(pulse * 255);
    if (!isPlayerTurn && !active) col.a = 50;
    DrawRectangleRounded({sx + 1, y + 1, segW - 2, barH - 2},
                          0.25f, 4, col);
    int tw = MeasureText(labels[i], 8);
    DrawText(labels[i], (int)(sx + segW / 2 - tw / 2), (int)(y + 7), 8,
             active ? WHITE : Color{175, 165, 135, 150});
    if (i < numPhases - 1)
      DrawLine((int)(sx + segW), (int)y,
               (int)(sx + segW), (int)(y + barH), {75, 65, 45, 140});
    // Small phase flag above the bar
    DrawPhaseFlagIcon(sx + segW / 2.0f, y, phaseColors[i], active);
  }
}

// ── Sliding turn announcement flag banner ─────────────────────────────────────
static void DrawTurnBanner(float timer, int who) {
  if (timer <= 0.f) return;
  const float totalDur = 3.2f;
  const float slideIn  = 0.32f;
  const float fadeOut  = 0.55f;
  float elapsed  = totalDur - timer;

  // Slide progress 0→1
  float slideT   = Clamp(elapsed / slideIn, 0.f, 1.f);
  float eased    = slideT * slideT * (3.f - 2.f * slideT); // smoothstep

  // Alpha: fade out in last fadeOut seconds
  float alpha    = (timer < fadeOut) ? timer / fadeOut : 1.0f;
  unsigned char a8 = (unsigned char)(alpha * 255);

  const float bW = 260.f, bH = 52.f;
  const float cy = (float)SCREEN_H / 2.f - bH / 2.f - 30.f;

  bool playerTurn = (who == 0);
  // Slide from the correct side
  float targetX  = (float)SCREEN_W / 2.f - bW / 2.f;
  float offX     = playerTurn ? -(bW + 60.f) : ((float)SCREEN_W + 60.f);
  float bX       = offX + (targetX - offX) * eased;

  // Drop shadow
  DrawRectangleRounded({bX + 5, cy + 5, bW + 28, bH}, 0.22f, 4, {0,0,0,(unsigned char)(alpha*80)});

  // Banner colors
  Color bgCol  = playerTurn ? Color{18, 55, 130, a8}  : Color{100, 22, 22, a8};
  Color rimCol = playerTurn ? Color{80, 160, 255, a8} : Color{255, 90, 70, a8};
  Color txtCol = {255, 240, 200, a8};

  // Banner body
  DrawRectangleRounded({bX, cy, bW, bH}, 0.22f, 4, bgCol);
  DrawRectangleRoundedLinesEx({bX, cy, bW, bH}, 0.22f, 4, 1.8f, rimCol);

  // Waving pointed tail
  float wave = sinf(g_time * 5.8f) * 4.f;
  if (playerTurn) {
    // Tail points left
    DrawTriangle({bX, cy}, {bX, cy + bH},
                 {bX - 26.f, cy + bH * 0.5f + wave}, bgCol);
    DrawLine((int)(bX - 26), (int)(cy), (int)(bX - 26), (int)(cy + bH),
             {(unsigned char)(rimCol.r/2), (unsigned char)(rimCol.g/2),
              (unsigned char)(rimCol.b/2), a8});
  } else {
    // Tail points right
    DrawTriangle({bX + bW, cy}, {bX + bW, cy + bH},
                 {bX + bW + 26.f, cy + bH * 0.5f + wave}, bgCol);
    DrawLine((int)(bX + bW + 26), (int)(cy), (int)(bX + bW + 26), (int)(cy + bH),
             {(unsigned char)(rimCol.r/2), (unsigned char)(rimCol.g/2),
              (unsigned char)(rimCol.b/2), a8});
  }

  // Flagpole — vertical bar on the leading edge
  Color poleCol = {200, 175, 95, a8};
  if (playerTurn) {
    DrawRectangle((int)(bX - 30), (int)(cy - 6), 5, (int)(bH + 12), poleCol);
    DrawCircle((int)(bX - 27), (int)(cy - 8), 5, {210, 185, 90, a8}); // finial orb
  } else {
    DrawRectangle((int)(bX + bW + 25), (int)(cy - 6), 5, (int)(bH + 12), poleCol);
    DrawCircle((int)(bX + bW + 27), (int)(cy - 8), 5, {210, 185, 90, a8});
  }

  // Main label
  const char *label    = playerTurn ? "YOUR TURN"       : "OPPONENT'S TURN";
  int         fontSize = playerTurn ? 22                 : 17;
  int         lw       = MeasureText(label, fontSize);
  DrawText(label, (int)(bX + bW / 2 - lw / 2), (int)(cy + 10), fontSize, txtCol);

  // Sub-label
  const char *sub = playerTurn ? "Draw & Play"  : "Waiting...";
  int subSz = 9;
  int sw    = MeasureText(sub, subSz);
  Color subCol = {rimCol.r, rimCol.g, rimCol.b, (unsigned char)(alpha * 170)};
  DrawText(sub, (int)(bX + bW / 2 - sw / 2), (int)(cy + 34), subSz, subCol);
}

// Orbiting +/- counter orbs around a card (screen-space)
static void DrawCounterOrbs(float sx, float sy, int power, int weak) {
  int total = power + weak;
  if (total <= 0) return;
  float radius    = 19.0f;
  float angleStep = (total > 1) ? (2.0f * PI / total) : 0.0f;
  float startAngle = g_time * 1.4f;
  int idx = 0;
  for (int p = 0; p < power && idx < 8; p++, idx++) {
    float a  = startAngle + idx * angleStep;
    float ox = cosf(a) * radius;
    float oy = sinf(a) * radius;
    DrawCircle((int)(sx + ox), (int)(sy + oy), 6.0f, {35, 155, 45, 210});
    DrawText("+", (int)(sx + ox - 4), (int)(sy + oy - 5), 10,
             {190, 255, 190, 255});
  }
  for (int w = 0; w < weak && idx < 8; w++, idx++) {
    float a  = startAngle + idx * angleStep;
    float ox = cosf(a) * radius;
    float oy = sinf(a) * radius;
    DrawCircle((int)(sx + ox), (int)(sy + oy), 6.0f, {155, 35, 45, 210});
    DrawText("-", (int)(sx + ox - 3), (int)(sy + oy - 5), 10,
             {255, 190, 190, 255});
  }
}

// Keyword icon badges rendered below the card name label (screen-space)
static void DrawKeywordBadges(float sx, float sy, const CardDef &cd) {
  float bx = sx - 30.0f;
  float by = sy + 20.0f;
  auto badge = [&](const char *kw, Color col) {
    int w = MeasureText(kw, 8);
    Color bg = {(unsigned char)(col.r / 4), (unsigned char)(col.g / 4),
                (unsigned char)(col.b / 4), 200};
    DrawRectangleRounded({bx, by, (float)(w + 6), 13}, 0.5f, 4, bg);
    DrawRectangleRoundedLinesEx({bx, by, (float)(w + 6), 13},
                                 0.5f, 4, 1.0f, col);
    DrawText(kw, (int)(bx + 3), (int)(by + 2), 8, col);
    bx += (float)(w + 10);
  };
  if (CardHasKeyword(cd, "fly"))      badge("FLY",  {175, 218, 255, 255});
  if (CardHasKeyword(cd, "dash"))     badge("DASH", {255, 178,  55, 255});
  if (CardHasKeyword(cd, "tenacity")) badge("TEN",  { 95, 255,  95, 255});
  if (CardHasKeyword(cd, "overrun"))  badge("OVR",  {255, 115,  75, 255});
}

// "Thinking..." pulsing indicator above AI portrait (shown while AI processes)
static void DrawAIThinkingIndicator() {
  if (g_aiThinkTimer <= 0.0f) return;
  int dotCount = (int)(g_time * 2.8f) % 4;
  const char *dotStr = dotCount == 0 ? "." : dotCount == 1 ? ".." :
                       dotCount == 2 ? "..." : "....";
  char buf[32];
  snprintf(buf, 32, "Thinking%s", dotStr);
  int tw = MeasureText(buf, 12);
  float bx = SCREEN_W / 2.0f - tw / 2.0f - 12;
  DrawRectangleRounded({bx, 76, (float)(tw + 24), 22}, 0.35f, 4,
                        {22, 17, 12, 215});
  DrawRectangleRoundedLinesEx({bx, 76, (float)(tw + 24), 22}, 0.35f, 4,
                               1.0f, {175, 148, 75, 195});
  DrawText(buf, (int)bx + 12, 80, 12, {200, 178, 118, 225});
}

// Draw arena burst particles (2D overlay — called after EndMode3D)
static void DrawArenaBursts() {
  for (int i = 0; i < ARENA_BURST_MAX; i++) {
    const ArenaBurst &b = g_arenaBursts[i];
    if (!b.active) continue;
    Color c = b.col;
    c.a = (unsigned char)(b.life * 195.0f);
    DrawRectangle((int)b.x, (int)b.y, (int)b.size, (int)b.size, c);
  }
}

// ── §CARDVIEW  Dynamic High-Fidelity TCG Card Renderer  ───────────────────────
// Layered GPU card renderer: gold filigree frame + art + foil + bloom pulse.
//
//   Layer 1 – Physical card body     CPU rounded rect + drop shadow
//   Layer 2 – Gold filigree frame    GLSL SDF (metallic, roughness 0.2)
//   Layer 3 – Art window             g_cardTextures sampled in fragment shader
//   Layer 4 – UI text overlays       DrawText calls on top of shader pass
//
// Hover-tilt: rlgl quad with per-vertex skew → parallax light catch on gold.
// ═════════════════════════════════════════════════════════════════════════════

// ── GLSL: Vertex (pass-through – tilt handled by skewed rlgl quad) ───────────
static const char *CV_VERT = R"GLSL(
#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
void main() {
    gl_Position  = mvp * vec4(vertexPosition, 1.0);
    fragTexCoord = vertexTexCoord;
    fragColor    = vertexColor;
}
)GLSL";

// ── GLSL: Fragment  ───────────────────────────────────────────────────────────
static const char *CV_FRAG = R"GLSL(
#version 330
in  vec2 fragTexCoord;
in  vec4 fragColor;
out vec4 finalColor;

uniform sampler2D texture0;
uniform float uTime;
uniform float uRarity;    // 0=common  1=rare  2=unique
uniform float uPlayable;  // 1 when player can afford card
uniform float uHoverX;    // hover tilt X  (-0.25 .. 0.25 rad)
uniform float uHoverY;    // hover tilt Y  (-0.25 .. 0.25 rad)

// ── SDF helpers ───────────────────────────────────────────────────────────────
float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// ── Gold metallic colour (PBR: metallic=1, roughness=0.2) ─────────────────────
vec3 goldLit(vec2 p) {
    // Desert-sun direction perturbed by hover tilt
    vec2 ldir = normalize(vec2(0.55 + uHoverY * 0.60,
                               0.70 - uHoverX * 0.60));
    float ndotl = dot(normalize(p + vec2(0.0, 0.25)), ldir);
    vec3 gBase  = vec3(0.82, 0.62, 0.08);
    vec3 gLight = vec3(1.00, 0.92, 0.50);
    vec3 gDark  = vec3(0.40, 0.28, 0.01);
    vec3 g = mix(gDark, mix(gBase, gLight,
                 clamp(ndotl * 0.8 + 0.5, 0.0, 1.0)), 0.9);
    // Sharp specular spike (metallic roughness 0.2)
    float spec = pow(max(0.0, ndotl), 38.0);
    g += vec3(1.0, 0.95, 0.65) * spec * 0.90;
    return g;
}

// ── Procedural filigree SDF (quarter-arcs + cross-hair per corner) ────────────
float filigreeAtCorner(vec2 uv, vec2 corner) {
    vec2 p = (uv - corner) * vec2(corner.x < 0.5 ? -1.0 : 1.0,
                                   corner.y < 0.5 ? -1.0 : 1.0) * 7.0;
    float d = 100.0;
    // Three concentric quarter-arcs
    for (int i = 0; i < 3; i++) {
        float r   = 0.45 + float(i) * 0.55;
        float arc = abs(length(p) - r) - 0.07;
        if (p.x >= -0.05 && p.y >= -0.05) d = min(d, arc);
    }
    // Diagonal cross-hair
    vec2 rp = vec2(p.x - p.y, p.x + p.y) * 0.7071;
    d = min(d, abs(rp.x) - 0.055);
    d = min(d, abs(rp.y) - 0.055);
    return d;
}

// ── Cheap 2-D value noise (foil hue-shift) ────────────────────────────────────
float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float noise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i),          hash21(i + vec2(1,0)), f.x),
               mix(hash21(i + vec2(0,1)), hash21(i + vec2(1,1)), f.x), f.y);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    return c.z * mix(K.xxx, clamp(abs(fract(c.xxx + K.xyz)*6.0 - K.www) - K.xxx,
                                  0.0, 1.0), c.y);
}

// ── Main ──────────────────────────────────────────────────────────────────────
void main() {
    vec2 uv = fragTexCoord;       // 0..1
    vec2 p  = uv * 2.0 - 1.0;    // -1..1

    // ── Layer 1: Rounded card body ───────────────────────────────────────────
    float cardSDF  = sdRoundBox(p, vec2(0.86, 0.94), 0.055);
    float cardMask = 1.0 - smoothstep(-0.004, 0.004, cardSDF);
    if (cardMask < 0.01) discard;

    // ── Layer 3: Art window (recessed centre) ────────────────────────────────
    float artSDF  = sdRoundBox(p - vec2(0.0, -0.06), vec2(0.64, 0.42), 0.035);
    float artMask = 1.0 - smoothstep(-0.004, 0.004, artSDF);

    // ── Layer 2: Frame band = card minus art window ──────────────────────────
    float frameMask = cardMask * (1.0 - artMask);

    // Gold colour with metallic PBR lighting
    vec3 goldCol = goldLit(p);

    // Filigree ornaments at all four corners
    vec2 corners[4];
    corners[0] = vec2(0.08, 0.07);  corners[1] = vec2(0.92, 0.07);
    corners[2] = vec2(0.08, 0.93);  corners[3] = vec2(0.92, 0.93);
    float filigreeSDF = 100.0;
    for (int i = 0; i < 4; i++)
        filigreeSDF = min(filigreeSDF, filigreeAtCorner(uv, corners[i]));
    float filigreeMask = (1.0 - smoothstep(-0.008, 0.008, filigreeSDF)) * frameMask;
    goldCol = mix(goldCol, goldCol * 1.55, filigreeMask);

    // Scrollwork name banner (top zone, tapered ends)
    float bannerMask = step(-0.94, p.y) * step(p.y, -0.55)
                     * step(-0.72, p.x) * step(p.x,  0.72)
                     * (1.0 - smoothstep(0.60, 0.72, abs(p.x)));
    float grain = noise2(uv * vec2(14.0, 4.0) + 0.5) * 0.18;
    vec3 bannerCol = vec3(0.16 + grain, 0.08 + grain * 0.5, 0.02);

    // Stat shield shapes
    // ATK: bottom-left crimson
    float atkSDF  = sdRoundBox(p - vec2(-0.55, 0.78), vec2(0.18, 0.10), 0.05);
    float atkMask = (1.0 - smoothstep(-0.004, 0.004, atkSDF)) * cardMask;
    float atkRim  = (1.0 - smoothstep(0.0, 0.018, abs(atkSDF))) * cardMask;
    // DEF: bottom-right forest-green
    float defSDF  = sdRoundBox(p - vec2( 0.55, 0.78), vec2(0.18, 0.10), 0.05);
    float defMask = (1.0 - smoothstep(-0.004, 0.004, defSDF)) * cardMask;
    float defRim  = (1.0 - smoothstep(0.0, 0.018, abs(defSDF))) * cardMask;
    // Cost pip: top-left crimson disc
    float costSDF  = length(p - vec2(-0.78, -0.82)) - 0.11;
    float costMask = (1.0 - smoothstep(-0.004, 0.004, costSDF)) * cardMask;

    // Dark leather text-box strip (lower centre)
    float tbMask = step(-0.48, p.x) * step(p.x,  0.48)
                 * step( 0.34, p.y) * step(p.y,  0.86) * cardMask;

    // ── Layer 3: Art texture sample with hover parallax ──────────────────────
    vec2 artUV     = uv + vec2(uHoverY * 0.012, uHoverX * 0.012) * artMask;
    vec4 artSample = texture(texture0, artUV);

    // ── Foil shimmer (rare / unique) ──────────────────────────────────────────
    float foilAmt = step(0.5, uRarity);
    float noiseF  = noise2(uv * 3.5 + vec2(uTime * 0.22, uTime * 0.15));
    vec3  foilCol = hsv2rgb(vec3(noiseF + uTime * 0.10, 0.75, 1.0));

    // ── Bloom pulse (playable card glow) ──────────────────────────────────────
    float pulse    = sin(uTime * 4.2) * 0.5 + 0.5;
    float edgeDist = 1.0 - smoothstep(0.0, 0.032, abs(cardSDF));
    vec3  bloomCol = vec3(1.0, 0.88, 0.12) * (uPlayable * pulse * edgeDist * 1.8);

    // ── Composite ─────────────────────────────────────────────────────────────
    // Start from art texture with warm vignette
    vec3 col = artSample.rgb;
    float vig = 1.0 - smoothstep(0.4, 0.9, length(p * vec2(0.6, 0.8)));
    col = mix(col * vec3(0.6, 0.45, 0.28), col, vig * 0.55 + 0.45);

    col = mix(col, goldCol, frameMask * 0.93);
    col = mix(col, bannerCol * 0.7 + goldCol * 0.3, bannerMask * frameMask * 0.88);
    col = mix(col, vec3(0.55, 0.07, 0.07), atkMask);
    col = mix(col, goldCol * 0.9,           atkRim);
    col = mix(col, vec3(0.07, 0.38, 0.12),  defMask);
    col = mix(col, goldCol * 0.9,           defRim);
    col = mix(col, vec3(0.50, 0.06, 0.06),  costMask);
    col = mix(col, vec3(0.11, 0.07, 0.03) + vec3(grain * 0.5),
              tbMask * (1.0 - atkMask) * (1.0 - defMask));
    col = mix(col, foilCol, artMask * foilAmt * 0.32);
    col += bloomCol;
    // Inner bevel highlight on art-window edge
    float bevel = (1.0 - smoothstep(0.0, 0.012, abs(artSDF - 0.01))) * frameMask;
    col = mix(col, goldCol * 1.3, bevel * 0.7);

    finalColor = vec4(col, cardMask);
}
)GLSL";

// CardView struct moved to forward declarations section (before DrawCardAnims)

// ── Shader globals ────────────────────────────────────────────────────────────
static Shader g_cardShader    = {0};
static int    g_cvLocTime     = -1;
static int    g_cvLocRarity   = -1;
static int    g_cvLocPlayable = -1;
static int    g_cvLocHoverX   = -1;
static int    g_cvLocHoverY   = -1;

static void InitCardShaders() {
  g_cardShader    = LoadShaderFromMemory(CV_VERT, CV_FRAG);
  g_cvLocTime     = GetShaderLocation(g_cardShader, "uTime");
  g_cvLocRarity   = GetShaderLocation(g_cardShader, "uRarity");
  g_cvLocPlayable = GetShaderLocation(g_cardShader, "uPlayable");
  g_cvLocHoverX   = GetShaderLocation(g_cardShader, "uHoverX");
  g_cvLocHoverY   = GetShaderLocation(g_cardShader, "uHoverY");
}

static void UnloadCardShaders() { UnloadShader(g_cardShader); }

// ── g_handCardViews ───────────────────────────────────────────────────────────
static CardView g_handCardViews[MAX_HAND];

static void SyncHandCardViews(const MatchPlayer &pl) {
  for (int i = 0; i < pl.handSize && i < MAX_HAND; i++) {
    int cid = pl.hand[i];
    if (g_handCardViews[i].cardId != cid)
      g_handCardViews[i].Init(cid, /*doEnter=*/true);
    g_handCardViews[i].isPlayable = (GetCard(cid).cost <= pl.coins);
  }
}

// ── DrawCardView ──────────────────────────────────────────────────────────────
// rect    : destination screen rectangle
// view    : per-card state (tilt, rarity, playable flag, atk/def)
// artTexId: which slot in g_cardTextures[] to use as art layer
static void DrawCardView(Rectangle rect, const CardView &view, int artTexId) {
  const CardDef &cd = GetCard(view.cardId);
  float W = rect.width, H = rect.height;
  float X = rect.x,     Y = rect.y;

  // ── Zone proportions ─────────────────────────────────────────────────────
  float padX  = W * 0.055f;
  float iX    = X + padX, iW = W - padX * 2.f;
  float nameH = H * 0.135f;                      // 0%–13.5%  name band
  float artT  = Y + nameH;                        // art top
  float artH  = H * 0.415f;                       // art height (13.5%–55%)
  float artB  = artT + artH;                       // art bottom
  float typeH = H * 0.075f;                       // 55%–62.5% type strip
  float typeB = artB + typeH;
  float txtT  = typeB;                             // 62.5%–84% text box
  float txtB  = Y + H * 0.840f;
  float statT = txtB;                              // 84%–97%  stat band

  // Font sizes scaled by card height
  int fsName = Clamp((int)(H / 10.f),   7, 14);
  int fsStat = Clamp((int)(H /  7.5f),  9, 17);
  int fsType = Clamp((int)(H / 13.5f),  6, 10);
  int fsKw   = Clamp((int)(H / 12.5f),  6, 11);
  int fsEff  = Clamp((int)(H / 14.5f),  5,  9);

  // ── Drop shadow ───────────────────────────────────────────────────────────
  DrawRectangleRounded({X + 5, Y + 7, W, H}, 0.10f, 6, {0, 0, 0, 100});

  // ── Card body ─────────────────────────────────────────────────────────────
  DrawRectangleRounded(rect, 0.09f, 8, {22, 14, 7, 255});

  // ── Triple gold border (3 layers = depth illusion) ───────────────────────
  DrawRectangleRoundedLinesEx(rect, 0.09f, 8, 3.5f, {60, 32, 6, 255});            // dark shadow rim
  DrawRectangleRoundedLinesEx({X+1.5f,Y+1.5f,W-3,H-3}, 0.09f, 8, 2.0f, {215, 170, 45, 255}); // bright gold
  DrawRectangleRoundedLinesEx({X+3.5f,Y+3.5f,W-7,H-7}, 0.09f, 8, 1.0f, {255, 235, 130, 160}); // pale highlight
  DrawRectangleRoundedLinesEx({X+5.0f,Y+5.0f,W-10,H-10}, 0.09f, 8, 0.5f, {90, 55, 10, 120}); // inner dark

  // ── Name band ────────────────────────────────────────────────────────────
  // Gradient panel behind name
  DrawRectangleGradientV((int)iX, (int)Y, (int)iW, (int)(nameH + 1),
                          {40, 22, 6, 255}, {12, 6, 2, 255});
  // Gold separator line under name band
  DrawRectangle((int)iX, (int)(Y + nameH - 1), (int)iW, 2, {190, 148, 38, 220});
  DrawRectangle((int)iX, (int)(Y + nameH + 1), (int)iW, 1, {80, 50, 8, 140});

  // Card name (centered, gold, shadow beneath)
  {
    int nw = MeasureText(cd.name, fsName);
    int nx = (int)(X + W * 0.5f) - nw / 2;
    int ny = (int)(Y + nameH * 0.22f);
    DrawText(cd.name, nx + 1, ny + 1, fsName, {0, 0, 0, 160});
    DrawText(cd.name, nx,     ny,     fsName, {248, 218, 145, 255});
  }

  // ── Art window ───────────────────────────────────────────────────────────
  Rectangle artRect = {iX, artT, iW, artH};
  // Art backing
  DrawRectangle((int)iX, (int)artT, (int)iW, (int)artH, {15, 10, 4, 255});

  // Art texture or procedural fill
  {
    Texture2D &tex = (artTexId > 0 && artTexId < 200)
                     ? g_cardTextures[artTexId] : g_cardTextures[0];
    if (tex.id != 0) {
      Rectangle src2 = {0, 0, (float)CARD_TEX_W, (float)CARD_TEX_H};
      // Shader path for rare/unique (foil) — fallback also looks great
      if (g_cardShader.id > 0 && view.rarity >= 1) {
        float t  = g_time; float rr = (float)view.rarity;
        float pp = view.isPlayable ? 1.f : 0.f;
        float hx = view.hoverRotX, hy = view.hoverRotY;
        SetShaderValue(g_cardShader, g_cvLocTime,     &t,  SHADER_UNIFORM_FLOAT);
        SetShaderValue(g_cardShader, g_cvLocRarity,   &rr, SHADER_UNIFORM_FLOAT);
        SetShaderValue(g_cardShader, g_cvLocPlayable, &pp, SHADER_UNIFORM_FLOAT);
        SetShaderValue(g_cardShader, g_cvLocHoverX,   &hx, SHADER_UNIFORM_FLOAT);
        SetShaderValue(g_cardShader, g_cvLocHoverY,   &hy, SHADER_UNIFORM_FLOAT);
        BeginShaderMode(g_cardShader);
        DrawTexturePro(tex, src2, artRect, {0, 0}, 0.f, {255, 255, 255, 230});
        EndShaderMode();
      } else {
        DrawTexturePro(tex, src2, artRect, {0, 0}, 0.f, {255, 255, 255, 230});
      }
    }
  }

  // Art border: outer dark then inner gold (embossed look)
  DrawRectangleLinesEx(artRect, 2.5f, {50, 30, 8, 255});
  DrawRectangleLinesEx({iX+2.f, artT+2.f, iW-4.f, artH-4.f}, 1.5f, {190, 150, 40, 200});
  DrawRectangleLinesEx({iX+4.f, artT+4.f, iW-8.f, artH-8.f}, 0.5f, {255, 220, 100, 80});

  // Unplayable dimmer
  if (!view.isPlayable)
    DrawRectangle((int)iX, (int)artT, (int)iW, (int)artH, {0, 0, 0, 90});

  // ── Type strip ───────────────────────────────────────────────────────────
  // Color depends on card type: warm amber for unit, cool blue for support
  Color typeBase = cd.isUnit ? Color{55, 30, 8, 245} : Color{15, 38, 60, 245};
  DrawRectangle((int)iX, (int)artB, (int)iW, (int)typeH, typeBase);
  DrawRectangle((int)iX, (int)artB,       (int)iW, 1, {200, 155, 45, 180});
  DrawRectangle((int)iX, (int)(artB + typeH - 1), (int)iW, 1, {200, 155, 45, 150});

  // Type text: "UNIT - Subtype" or "SUPPORT"
  {
    char typeBuf[48];
    if (cd.isUnit && cd.subtype && cd.subtype[0])
      snprintf(typeBuf, 48, "Unit - %s", cd.subtype);
    else
      snprintf(typeBuf, 48, cd.isUnit ? "Unit" : "Support");
    int tw2 = MeasureText(typeBuf, fsType);
    DrawText(typeBuf, (int)(iX + iW * 0.5f - tw2 * 0.5f),
             (int)(artB + typeH * 0.15f), fsType, {225, 200, 140, 240});
  }

  // ── Text box ─────────────────────────────────────────────────────────────
  DrawRectangle((int)iX, (int)txtT, (int)iW, (int)(txtB - txtT), {10, 6, 3, 245});
  DrawRectangle((int)iX, (int)typeB, (int)iW, 1, {140, 105, 28, 160});

  if (H > 70.f) {
    float tbMaxW = iW - 4.f;
    float curY   = txtT + 2.f;
    float tbBot  = txtB - 2.f;

    // Keywords (teal italic-style pills)
    if (cd.keywords && cd.keywords[0]) {
      static const struct { const char *id; const char *label; } KW[] = {
        {"fly","Flight"},{"dash","Haste"},{"tenacity","Tenacity"},
        {"overrun","Trample"},{"harvest","Harvest"},{"dig","Dig"},{nullptr,nullptr}
      };
      char kwLine[80] = {};
      char kwTmp[64]; snprintf(kwTmp, 64, "%s", cd.keywords);
      char *p = kwTmp; bool first = true;
      while (*p) {
        char *e = p; while (*e && *e != ';') ++e;
        char sv = *e; *e = '\0';
        const char *lbl = p;
        for (int ki = 0; KW[ki].id; ki++)
          if (strcmp(p, KW[ki].id) == 0) { lbl = KW[ki].label; break; }
        if (!first) strncat(kwLine, ", ", sizeof(kwLine) - strlen(kwLine) - 1);
        strncat(kwLine, lbl, sizeof(kwLine) - strlen(kwLine) - 1);
        first = false; *e = sv; p = (*e) ? e + 1 : e;
      }
      strncat(kwLine, ".", sizeof(kwLine) - strlen(kwLine) - 1);
      DrawText(kwLine, (int)(iX + 2), (int)curY, fsKw, {95, 215, 185, 255});
      curY += fsKw + 3;
    }

    // Effect text — word-wrap
    if (cd.effect && cd.effect[0] && curY < tbBot) {
      char effTmp[128]; snprintf(effTmp, 128, "%s", cd.effect);
      char *p = effTmp;
      int lineH = fsEff + 2;
      int maxC  = (int)(tbMaxW / fmaxf((float)MeasureText("n", fsEff), 1.f)) + 2;
      if (maxC < 4) maxC = 4;
      while (*p && curY + lineH <= tbBot) {
        int len = (int)strlen(p);
        if (len <= maxC) {
          DrawText(p, (int)(iX + 2), (int)curY, fsEff, {210, 192, 158, 225});
          break;
        }
        int brk = maxC;
        for (int j = maxC; j > 1; j--) if (p[j] == ' ') { brk = j; break; }
        char row[128]; memcpy(row, p, brk); row[brk] = '\0';
        DrawText(row, (int)(iX + 2), (int)curY, fsEff, {210, 192, 158, 225});
        p += brk; if (*p == ' ') p++;
        curY += lineH;
      }
    }

    // Card number (bottom of text box, dim gray)
    {
      char numBuf[16]; snprintf(numBuf, 16, "%d/%d", cd.id, NUM_ALL_CARDS);
      int nw2 = MeasureText(numBuf, fsEff - 1);
      DrawText(numBuf, (int)(X + W * 0.5f - nw2 * 0.5f),
               (int)(txtB - fsEff - 1), fsEff - 1, {120, 100, 72, 160});
    }
  }

  // ── Stat band divider line ────────────────────────────────────────────────
  DrawRectangle((int)iX, (int)statT, (int)iW, 1, {185, 144, 35, 210});

  // ── Cost gem (top-left corner, overlapping border) ───────────────────────
  {
    float gR  = W * 0.115f;
    float gCX = X + padX * 0.35f + gR;
    float gCY = Y + nameH * 0.5f;
    // Outer ring
    DrawCircle((int)gCX, (int)gCY, (int)(gR + 2.f), {50, 24, 4, 255});
    // Deep blue gem
    DrawCircle((int)gCX, (int)gCY, (int)(gR),         {28, 55, 130, 255});
    DrawCircle((int)gCX, (int)gCY, (int)(gR * 0.72f), {45, 90, 190, 255});
    DrawCircle((int)gCX, (int)gCY, (int)(gR * 0.40f), {85, 145, 235, 255});
    // Glint
    DrawCircle((int)(gCX - gR * 0.25f), (int)(gCY - gR * 0.28f),
               (int)(gR * 0.18f), {210, 230, 255, 180});
    // Cost number
    char costB[4]; snprintf(costB, 4, "%d", view.cost);
    int cfs = Clamp((int)(gR * 1.5f), 8, 15);
    int cw  = MeasureText(costB, cfs);
    DrawText(costB, (int)(gCX - cw * 0.5f + 1), (int)(gCY - cfs * 0.5f + 1), cfs, {0, 0, 0, 140});
    DrawText(costB, (int)(gCX - cw * 0.5f),     (int)(gCY - cfs * 0.5f),     cfs, {240, 230, 200, 255});
  }

  // ── Rarity gem (top-right corner) ────────────────────────────────────────
  if (view.rarity > 0) {
    float gR  = W * 0.100f;
    float gCX = X + W - padX * 0.35f - gR;
    float gCY = Y + nameH * 0.5f;
    float sh  = sinf(g_time * 5.f) * 0.5f + 0.5f;

    Color c1 = (view.rarity == 2) ? Color{130, 40, 20, 255}  : Color{120, 90, 10, 255};
    Color c2 = (view.rarity == 2) ? Color{220, 80, 40, 255}  : Color{210, 168, 40, 255};
    Color c3 = (view.rarity == 2) ? Color{255, 145, 80, 255} : Color{255, 230, 100, 255};

    DrawCircle((int)gCX, (int)gCY, (int)(gR + 2.f), {50, 24, 4, 255});
    DrawCircle((int)gCX, (int)gCY, (int)(gR),         c1);
    DrawCircle((int)gCX, (int)gCY, (int)(gR * 0.68f), c2);
    DrawCircle((int)gCX, (int)gCY, (int)(gR * 0.36f), c3);
    if (view.rarity == 2) {
      // Animated shimmer for unique cards
      Color shimmer = {255, (unsigned char)(190 + (int)(65 * sh)),
                            (unsigned char)(80  + (int)(120 * sh)), 200};
      DrawCircle((int)(gCX - gR * 0.2f), (int)(gCY - gR * 0.25f),
                 (int)(gR * 0.22f), shimmer);
    }
    // Rarity label
    const char *rlbl = (view.rarity == 2) ? "U" : "R";
    int rfs = Clamp((int)(gR * 1.4f), 7, 13);
    int rw  = MeasureText(rlbl, rfs);
    DrawText(rlbl, (int)(gCX - rw * 0.5f), (int)(gCY - rfs * 0.5f), rfs, {255, 245, 210, 255});

    // Star pips below gem
    int stars = view.rarity + 1;
    float sY = gCY + gR + 3.f, sGap = 5.5f, sW = 3.5f;
    float sX0 = gCX - (stars * sGap - (sGap - sW)) * 0.5f;
    Color sc = (view.rarity == 2) ? Color{220, 130, 255, 235} : Color{220, 185, 55, 225};
    for (int s = 0; s < stars; s++)
      DrawRectangle((int)(sX0 + s * sGap), (int)sY, (int)sW, (int)sW, sc);
  }

  // ── ATK / DEF stat badges (bottom band) ──────────────────────────────────
  if (cd.isUnit) {
    float bH  = H * 0.115f;
    float bW  = iW * 0.425f;
    float bY  = statT + (H * 0.13f - bH) * 0.5f;

    // ATK badge — crimson left
    Rectangle atkR = {iX, bY, bW, bH};
    DrawRectangleRounded(atkR, 0.30f, 4, {75, 12, 10, 255});
    DrawRectangleRounded(atkR, 0.30f, 4, {130, 25, 22, 200});
    DrawRectangleRoundedLinesEx(atkR, 0.30f, 4, 1.5f, {215, 70, 50, 255});
    DrawRectangleRoundedLinesEx({atkR.x+1,atkR.y+1,atkR.width-2,atkR.height-2},
                                 0.30f, 4, 0.5f, {255, 160, 140, 80});

    int sfs = Clamp((int)(bH * 0.65f), 7, 12);
    DrawText("ATK", (int)(atkR.x + 4), (int)(atkR.y + 2),           sfs - 2, {255, 150, 130, 210});
    char buf[8]; snprintf(buf, 8, "%d", view.atk);
    int aw = MeasureText(buf, sfs);
    DrawText(buf, (int)(atkR.x + bW - aw - 5), (int)(atkR.y + (bH - sfs) * 0.5f), sfs, {255, 242, 200, 255});

    // DEF badge — teal right
    Rectangle defR = {iX + iW - bW, bY, bW, bH};
    DrawRectangleRounded(defR, 0.30f, 4, {8, 45, 35, 255});
    DrawRectangleRounded(defR, 0.30f, 4, {16, 72, 58, 200});
    DrawRectangleRoundedLinesEx(defR, 0.30f, 4, 1.5f, {40, 190, 150, 255});
    DrawRectangleRoundedLinesEx({defR.x+1,defR.y+1,defR.width-2,defR.height-2},
                                 0.30f, 4, 0.5f, {100, 255, 210, 80});

    DrawText("DEF", (int)(defR.x + 4), (int)(defR.y + 2),           sfs - 2, {80, 230, 195, 210});
    snprintf(buf, 8, "%d", view.def);
    int dw = MeasureText(buf, sfs);
    DrawText(buf, (int)(defR.x + bW - dw - 5), (int)(defR.y + (bH - sfs) * 0.5f), sfs, {195, 255, 225, 255});
  }

  // ── Playable green glow ring ──────────────────────────────────────────────
  if (view.isPlayable) {
    float pulse = sinf(g_time * 4.5f) * 0.28f + 0.72f;
    unsigned char ga = (unsigned char)(160 * pulse);
    DrawRectangleRoundedLinesEx({X - 2, Y - 2, W + 4, H + 4}, 0.09f, 8, 2.5f,
      {50, (unsigned char)(205 * pulse), 100, ga});
  }
}
// ── end §CARDVIEW ─────────────────────────────────────────────────────────────

// ── Draw Match Scene ────────────────────────────────────────────────────────
static void DrawMatchScene() {
  GameMatch &m = g_match;
  MatchPlayer &human = m.players[0];
  MatchPlayer &ai    = m.players[1];

  // ── Hover state detection ─────────────────────────────────────────────────
  Vector2 mouse = GetMousePosition();
  g_matchHoverHand       = -1;
  g_matchHoverField      = -1;
  g_matchHoverEnemyField = -1;
  for (int i = 0; i < human.handSize; i++) {
    Rectangle r = GetHandCardRect(i, human.handSize);
    if (CheckCollisionPointRec(mouse, r)) { g_matchHoverHand = i; break; }
  }
  for (int i = 0; i < human.fieldSize; i++) {
    Rectangle r = GetFieldScreenRect(0, i, human.fieldSize, 60, 45);
    if (CheckCollisionPointRec(mouse, r)) { g_matchHoverField = i; break; }
  }
  for (int i = 0; i < ai.fieldSize; i++) {
    Rectangle r = GetFieldScreenRect(1, i, ai.fieldSize, 60, 45);
    if (CheckCollisionPointRec(mouse, r)) { g_matchHoverEnemyField = i; break; }
  }

  // ── Combat targeting arrow: track selected attacker to mouse ─────────────
  if (m.turn == 0 && m.phase == PHASE_ACTIVATE && m.selectedFieldIdx >= 0) {
    g_arrowActive = true;
    Vector3 srcPos = GetFieldSlotPos(0, m.selectedFieldIdx, human.fieldSize);
    g_arrowSrc = GetWorldToScreen(srcPos, g_matchCam);
    g_arrowDst = mouse;
  } else {
    g_arrowActive = false;
  }

  // ══════════════════════════════════════════════════════════════════════════
  //  2D: Desert arena background (rendered first, under 3D scene)
  // ══════════════════════════════════════════════════════════════════════════
  DrawArenaBackground();

  // ══════════════════════════════════════════════════════════════════════════
  //  3D TABLE SCENE  (60-degree perspective, desert hardwood table)
  // ══════════════════════════════════════════════════════════════════════════
  BeginMode3D(g_matchCam);

  // ── Table body (desert hardwood) ──────────────────────────────────────────
  DrawCube({0, -1.2f, 0.5f}, 28, 2.0f, 20, {68, 40, 17, 255});
  DrawCubeWires({0, -1.2f, 0.5f}, 28, 2.0f, 20, {90, 55, 24, 255});

  // Table legs (key light: left side slightly brighter)
  float legH = 4.0f, legR = 0.40f;
  DrawCylinder({-12.5f, -2.2f - legH * 0.5f, -8.5f}, legR, legR, legH, 6, {52, 30, 11, 255});
  DrawCylinder({ 12.5f, -2.2f - legH * 0.5f, -8.5f}, legR, legR, legH, 6, {46, 26,  9, 255});
  DrawCylinder({-12.5f, -2.2f - legH * 0.5f,  9.5f}, legR, legR, legH, 6, {52, 30, 11, 255});
  DrawCylinder({ 12.5f, -2.2f - legH * 0.5f,  9.5f}, legR, legR, legH, 6, {46, 26,  9, 255});

  // Felt surface
  DrawCube({0, -0.15f, 0.5f}, 26, 0.08f, 18, {20, 56, 30, 255});
  // Gold edge trim
  DrawCubeWires({0, -0.10f, 0.5f}, 26.2f, 0.12f, 18.2f, {182, 152, 62, 200});
  // Centre dividing gold strip
  DrawCube({0, -0.10f, 0.5f}, 24, 0.02f, 0.09f, {182, 152, 62, 190});

  // Zone area felt shading
  DrawCube({0, -0.12f, -2.5f}, 18, 0.01f, 4.0f, {15, 46, 25, 255});
  DrawCube({0, -0.12f,  3.5f}, 18, 0.01f, 4.0f, {15, 46, 25, 255});

  // Corner markers
  float cmY = -0.09f;
  DrawSphere({-9.0f, cmY, -4.5f}, 0.12f, {182, 152, 62, 148});
  DrawSphere({ 9.0f, cmY, -4.5f}, 0.12f, {182, 152, 62, 148});
  DrawSphere({-9.0f, cmY,  5.5f}, 0.12f, {182, 152, 62, 148});
  DrawSphere({ 9.0f, cmY,  5.5f}, 0.12f, {182, 152, 62, 148});

  // ── Slot depressions (5 per side, always visible) ─────────────────────────
  DrawSlotDepressions(0, human.fieldSize);
  DrawSlotDepressions(1, ai.fieldSize);

  // Card slab dimensions
  const float CW = 2.2f, CDEP = 3.0f, CTHK = 0.14f;

  // ── AI field units ─────────────────────────────────────────────────────────
  for (int i = 0; i < ai.fieldSize; i++) {
    FieldUnit &fu = ai.field[i];
    Vector3 pos3  = GetFieldSlotPos(1, i, ai.fieldSize);
    const CardDef &cd = GetCard(fu.cardId);
    bool hasFly  = CardHasKeyword(cd, "fly");
    bool hasDash = CardHasKeyword(cd, "dash");
    Color frame  = fu.isDefender ? Color{55, 55, 200, 255} : Color{200, 162, 72, 255};
    if (i == g_matchHoverEnemyField) frame = {255, 100, 80, 255};
    DrawCard3DUnit(pos3, CW, CDEP, CTHK, fu.cardId, fu.activated,
                   (i == g_matchHoverEnemyField), hasFly, hasDash, frame);
  }

  // ── Player field units ────────────────────────────────────────────────────
  for (int i = 0; i < human.fieldSize; i++) {
    FieldUnit &fu = human.field[i];
    Vector3 pos3  = GetFieldSlotPos(0, i, human.fieldSize);
    const CardDef &cd = GetCard(fu.cardId);
    bool hasFly  = CardHasKeyword(cd, "fly");
    bool hasDash = CardHasKeyword(cd, "dash");
    bool sel     = (m.selectedFieldIdx == i);
    Color frame  = (fu.canActivate && !fu.activated)
                       ? Color{255, 222, 82, 255} : Color{122, 102, 62, 255};
    if (fu.isDefender) frame = {62, 102, 222, 255};
    DrawCard3DUnit(pos3, CW, CDEP, CTHK, fu.cardId, fu.activated,
                   sel, hasFly, hasDash, frame);
  }

  // ── AI face-down hand cards (top edge of table, patterned card backs) ───────
  for (int i = 0; i < ai.handSize; i++) {
    float spacing = 1.5f;
    float startX  = -(ai.handSize - 1) * spacing * 0.5f;
    float cx = startX + i * spacing;
    float cz = -7.5f;
    // Card body
    DrawCube({cx, -0.05f, cz}, 1.25f, 0.10f, 1.85f, {42, 28, 16, 255});
    // Card back design — two crossing diagonal lines on top face
    float topY = 0.001f;
    float hw = 0.55f, hd = 0.88f;
    // Border strip
    rlPushMatrix();
    rlTranslatef(cx, topY, cz);
    DrawCube({0, 0, 0}, 1.25f, 0.001f, 1.85f, {55, 38, 22, 255});
    DrawCube({0, 0.001f, 0}, 1.05f, 0.001f, 1.65f, {30, 18, 10, 255});
    // Inner diamond pattern
    DrawCube({0, 0.002f, 0}, 0.70f, 0.001f, 0.70f, {55, 38, 22, 200});
    rlPopMatrix();
    // Gold frame wire
    DrawCubeWires({cx, -0.05f, cz}, 1.28f, 0.12f, 1.88f, {120, 95, 42, 200});
  }

  // ── Deck stacks (player + AI, right side) ────────────────────────────────
  auto drawDeckStack = [](float cx, float cz, int deckSz) {
    if (deckSz <= 0) return;
    int layers = (deckSz > 6) ? 6 : deckSz;
    for (int i = 0; i < layers; i++)
      DrawCube({cx, -0.05f + i * 0.06f, cz}, 1.8f, 0.05f, 2.5f,
               {55, 38, 22, 255});
    DrawCubeWires({cx, -0.05f + (layers - 1) * 0.06f, cz},
                  1.8f, 0.05f, 2.5f, {122, 102, 62, 255});
  };
  drawDeckStack(10.5f,  5.5f, human.deckSize);   // player near camera = bottom
  drawDeckStack(10.5f, -6.5f, ai.deckSize);      // AI far = top

  // ── Graveyard stacks with top card face texture ───────────────────────────
  auto drawGraveStack = [&](float cx, float cz, MatchPlayer &mp) {
    if (mp.graveSize <= 0) return;
    int layers = (mp.graveSize > 6) ? 6 : mp.graveSize;
    for (int i = 0; i < layers; i++)
      DrawCube({cx, -0.05f + i * 0.06f, cz}, 1.8f, 0.05f, 2.5f,
               {46, 22, 46, 255});
    DrawCubeWires({cx, -0.05f + (layers - 1) * 0.06f, cz},
                  1.8f, 0.05f, 2.5f, {102, 62, 102, 255});
    // Top card face texture
    float topY = -0.05f + (layers - 1) * 0.06f;
    int topId = mp.grave[mp.graveSize - 1];
    DrawCardFaceTexture({cx, topY, cz}, 0.85f, 1.2f, topId, {255, 200, 200, 215});
  };
  drawGraveStack(-10.5f,  5.5f, human);   // player near camera = bottom
  drawGraveStack(-10.5f, -6.5f, ai);      // AI far = top

  // ── Oblivion exile portals (glowing, left side) ───────────────────────────
  DrawOblivionPortal3D({-10.5f, 0.08f,  7.8f});   // player = near camera = bottom
  if (human.oblivionSize > 0)
    DrawCardFaceTexture({-10.5f, 0.20f,  7.8f}, 0.68f, 0.95f,
                        human.oblivion[human.oblivionSize - 1],
                        {210, 110, 255, 185});

  DrawOblivionPortal3D({-10.5f, 0.08f, -8.8f});   // AI = far = top
  if (ai.oblivionSize > 0)
    DrawCardFaceTexture({-10.5f, 0.20f, -8.8f}, 0.68f, 0.95f,
                        ai.oblivion[ai.oblivionSize - 1],
                        {210, 110, 255, 185});

  // Ambient decorative coins (desert flavour)
  DrawSphere({-12.0f, 0.10f, 0.5f}, 0.26f, {202, 182, 62, 200});
  DrawSphere({-12.3f, 0.08f, 0.9f}, 0.18f, {202, 182, 62, 150});

  // ── Arena torch stands + chandelier ────────────────────────────────────────
  DrawArenaTorches();

  EndMode3D();

  // ══════════════════════════════════════════════════════════════════════════
  //  2D HUD OVERLAY
  // ══════════════════════════════════════════════════════════════════════════

  // ── Visual phase bar (top centre) ─────────────────────────────────────────
  DrawPhaseBar(m.phase, m.turn == 0);

  // ── Field card overlays: compact stat badge + keywords + orbs ──────────────
  // (card name removed — it's already rendered on the 3D card face texture)
  for (int side = 0; side < 2; side++) {
    MatchPlayer &mp = m.players[side];
    for (int i = 0; i < mp.fieldSize; i++) {
      FieldUnit &fu = mp.field[i];
      const CardDef &cd = GetCard(fu.cardId);
      Vector3 pos3 = GetFieldSlotPos(side, i, mp.fieldSize);
      if (CardHasKeyword(cd, "fly")) pos3.y += 0.45f;
      Vector2 sp = GetWorldToScreen(pos3, g_matchCam);

      int ea = fu.curAtk + fu.powerCounters - fu.weakCounters + fu.bonusAtk;
      int ed = fu.curDef + fu.powerCounters - fu.weakCounters + fu.bonusDef;
      if (ea < 0) ea = 0;
      if (ed < 0) ed = 0;

      // ── ATK / DEF pill badge ──────────────────────────────────────────────
      // Left shield: ATK (red-orange)
      {
        char buf[8]; snprintf(buf, 8, "%d", ea);
        int w = MeasureText(buf, 11);
        float bx = sp.x - 28 - w * 0.5f, by = sp.y + 14;
        Color bg = (fu.powerCounters > 0) ? Color{20,60,15,220}
                 : (fu.weakCounters  > 0) ? Color{60,15,15,220}
                                          : Color{65,20,18,215};
        DrawRectangleRounded({bx, by, (float)(w + 10), 16}, 0.55f, 4, bg);
        DrawRectangleRoundedLinesEx({bx, by, (float)(w + 10), 16}, 0.55f, 4,
                                     1.0f, {220, 100, 80, 200});
        DrawText(buf, (int)(bx + 5), (int)(by + 2), 11,
                 {255, 190, 160, 255});
      }
      // Right shield: DEF (blue-green)
      {
        char buf[8]; snprintf(buf, 8, "%d", ed);
        int w = MeasureText(buf, 11);
        float bx = sp.x + 18 - w * 0.5f, by = sp.y + 14;
        Color bg = (fu.powerCounters > 0) ? Color{15,55,20,220}
                 : (fu.weakCounters  > 0) ? Color{55,15,15,220}
                                          : Color{15,40,62,215};
        DrawRectangleRounded({bx, by, (float)(w + 10), 16}, 0.55f, 4, bg);
        DrawRectangleRoundedLinesEx({bx, by, (float)(w + 10), 16}, 0.55f, 4,
                                     1.0f, {80, 180, 240, 200});
        DrawText(buf, (int)(bx + 5), (int)(by + 2), 11,
                 {170, 225, 255, 255});
      }

      // Keyword badges
      DrawKeywordBadges(sp.x, sp.y - 8, cd);

      // Defender badge (blue pill above card)
      if (fu.isDefender) {
        DrawRectangleRounded({sp.x - 20, sp.y - 46, 40, 14}, 0.5f, 4,
                              {14, 18, 78, 220});
        DrawRectangleRoundedLinesEx({sp.x - 20, sp.y - 46, 40, 14},
                                     0.5f, 4, 1.0f, {100, 148, 255, 255});
        DrawText("GUARD", (int)sp.x - 16, (int)sp.y - 44, 8,
                 {140, 185, 255, 255});
      }

      // Tapped label
      if (fu.activated) {
        int tw = MeasureText("TAPPED", 8);
        DrawRectangleRounded({sp.x - tw/2.f - 4, sp.y + 33, (float)(tw + 8), 13},
                              0.5f, 4, {45, 18, 18, 200});
        DrawText("TAPPED", (int)sp.x - tw / 2, (int)sp.y + 35, 8,
                 {200, 80, 80, 200});
      }

      // Orbiting power / weakness tokens
      DrawCounterOrbs(sp.x, sp.y - 14, fu.powerCounters, fu.weakCounters);
    }
  }

  // ── Zone count badges projected from 3D ──────────────────────────────────
  {
    auto zBadge = [&](Vector3 wp, const char *label, int val, Color bg, Color fg) {
      Vector2 sp = GetWorldToScreen(wp, g_matchCam);
      char buf[20]; snprintf(buf, 20, "%s%d", label, val);
      int tw = MeasureText(buf, 9);
      DrawRectangleRounded({sp.x - tw/2.f - 5, sp.y - 10, (float)(tw + 10), 15},
                            0.5f, 4, bg);
      DrawText(buf, (int)(sp.x - tw/2), (int)(sp.y - 9), 9, fg);
    };
    zBadge({ 10.5f, 0.5f,  5.5f}, "DECK:", human.deckSize,
           {28,22,14,215}, {200, 180, 130, 255});
    zBadge({-10.5f, 0.5f,  5.5f}, "GRV:",  human.graveSize,
           {28,14,28,215}, {185, 120, 185, 255});
    zBadge({ 10.5f, 0.5f, -6.5f}, "DECK:", ai.deckSize,
           {22,18,12,190}, {180, 160, 118, 230});
    zBadge({-10.5f, 0.5f, -6.5f}, "GRV:",  ai.graveSize,
           {24,12,24,190}, {165, 100, 165, 230});
    zBadge({-10.5f, 0.5f,  7.8f}, "OBL:",  human.oblivionSize,
           {22,10,38,215}, {165,  82, 210, 255});
    zBadge({-10.5f, 0.5f, -8.8f}, "OBL:",  ai.oblivionSize,
           {18, 8,32,190}, {145,  65, 190, 230});
  }

  // ── Hand zone: radial fan at bottom with gold-trim matte backdrop ──────────
  DrawRectangleGradientV(0, SCREEN_H - 178, SCREEN_W, 32,
                         {0, 0, 0, 0}, {10, 6, 4, 245});
  DrawRectangle(0, SCREEN_H - 146, SCREEN_W, 146, {10, 6, 4, 248});
  // Gold separator rule at top of hand zone
  DrawRectangle(0, SCREEN_H - 146, SCREEN_W, 1, {182, 152, 62, 160});
  // Subtle inner highlight strip
  DrawRectangleGradientV(0, SCREEN_H - 145, SCREEN_W, 12,
                         {182, 152, 62, 25}, {0, 0, 0, 0});

  // Sync live CardView states (dynamic atk/def, playable flag)
  SyncHandCardViews(human);
  {
    Vector2 mouse2 = GetMousePosition();
    float fanCenter = (human.handSize - 1) * 0.5f;
    for (int i = 0; i < human.handSize && i < MAX_HAND; i++) {
      Rectangle r = GetHandCardRect(i, human.handSize);
      r.x += g_hspringOff[i];

      bool hov = (i == g_matchHoverHand);

      // Update hover-tilt spring
      Vector2 mrel = {mouse2.x - (r.x + r.width  * 0.5f),
                      mouse2.y - (r.y + r.height * 0.5f)};
      g_handCardViews[i].Update(GetFrameTime(), hov, mrel);

      // Fan pseudo-rotation: blend base fan-tilt into hoverRotX so the
      // parallelogram skew in DrawCardView gives a natural "fanned" look.
      float fanTilt = (i - fanCenter) /
                      fmaxf((float)(human.handSize - 1), 1.0f) * 0.55f;
      CardView fv = g_handCardViews[i];
      fv.hoverRotX += fanTilt * (hov ? 0.20f : 1.0f);  // flatten on hover

      // Scale: 1.4× on hover
      float scl   = hov ? 1.4f : fv.scale;
      Rectangle drawR = r;
      if (hov) {
        drawR = {r.x - r.width  * (scl - 1.0f) * 0.5f,
                 r.y - r.height * (scl - 1.0f),
                 r.width  * scl,
                 r.height * scl};
      }

      // Skip dragged card's original position while actively dragging
      if (i == g_dragCardIdx && g_dragActive) continue;

      // Keyboard selection highlight
      if (i == g_kbHandSel && g_zoomedCard < 0) {
        DrawRectangleRoundedLinesEx(drawR, 0.12f, 4, 3.0f, {255, 230, 60, 200});
      }

      // Full layered GPU render + text overlays
      DrawCardView(drawR, fv, human.hand[i]);
    }
  }

  // ── Drag-drop visual: card follows cursor + drop zone glow ─────────────────
  if (g_dragCardIdx >= 0 && g_dragActive && g_dragCardIdx < human.handSize) {
    // Drop zone indicator (player field area)
    bool inField = (g_dragPos.y < (float)(SCREEN_H - 150));
    Color dzCol = inField ? Color{80, 220, 80, (unsigned char)(110 + (int)(60*sinf(g_time*5.f)))}
                          : Color{200, 80, 80, 80};
    DrawRectangleRoundedLinesEx({SCREEN_W * 0.08f, SCREEN_H * 0.42f,
                                  SCREEN_W * 0.84f, SCREEN_H * 0.20f},
                                 0.08f, 6, 3.0f, dzCol);
    if (inField) {
      DrawText("RELEASE TO PLAY", SCREEN_W/2 - 55, (int)(SCREEN_H * 0.50f), 10,
               {120, 255, 120, 180});
    }
    // Card ghost following mouse
    CardView dv; dv.Init(human.hand[g_dragCardIdx]);
    dv.isPlayable = true;
    const float dw = 88 * 1.15f, dh = 126 * 1.15f;
    DrawCardView({g_dragPos.x - dw*0.5f, g_dragPos.y - dh*0.6f, dw, dh},
                 dv, human.hand[g_dragCardIdx]);
  }

  // ── Zoomed card inspect panel ──────────────────────────────────────────────
  if (g_zoomedCard >= 0 && g_zoomedCard < human.handSize) {
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, {0, 0, 0, 170});
    const float zw = 220.f, zh = 316.f;
    Rectangle zr = {SCREEN_W * 0.5f - zw * 0.5f, SCREEN_H * 0.5f - zh * 0.5f, zw, zh};
    CardView zv; zv.Init(human.hand[g_zoomedCard]); zv.isPlayable = true;
    DrawCardView(zr, zv, human.hand[g_zoomedCard]);
    DrawText("E/Enter: Play   Esc/Right-click: Close",
             (int)(zr.x + zw * 0.5f) - 90, (int)(zr.y + zh + 10), 10,
             {180, 162, 122, 220});
    // Show card number hint (1-9)
    if (g_zoomedCard < 9) {
      char hint[4]; snprintf(hint, 4, "[%d]", g_zoomedCard + 1);
      DrawText(hint, (int)(zr.x + 4), (int)(zr.y - 16), 11, {180, 162, 90, 200});
    }
  }

  // ── Opponent hand count ───────────────────────────────────────────────────
  {
    char buf[32]; snprintf(buf, 32, "Opponent Hand: %d", ai.handSize);
    DrawText(buf, SCREEN_W / 2 - 50, 34, 12, {180, 162, 122, 200});
  }

  // ── TCG Portrait Panels — glass-matte style ──────────────────────────────
  auto DrawHUDPanel = [](float px, float py, int life, int coins,
                         bool isPlayer) {
    float pw = 196.f, ph = 54.f;
    // Outer ambient glow
    DrawRectangleRoundedLinesEx({px - 2, py - 2, pw + 4, ph + 4},
                                 0.35f, 4, 2.5f,
                                 isPlayer ? Color{182, 152, 62, 60}
                                          : Color{182,  82, 62, 60});
    // Main body — deep matte
    DrawRectangleRounded({px, py, pw, ph}, 0.28f, 4, {12, 8, 5, 248});
    // Inner glass highlight (top gradient strip)
    DrawRectangleGradientV((int)px + 2, (int)py + 2, (int)pw - 4, 18,
                            {255, 255, 255, 18}, {0, 0, 0, 0});
    // Gold border
    Color borderCol = isPlayer ? Color{182, 152, 62, 210}
                               : Color{182,  82, 62, 210};
    DrawRectangleRoundedLinesEx({px, py, pw, ph}, 0.28f, 4, 1.5f, borderCol);

    // ── Portrait circle ───────────────────────────────────────────────────
    float cx = px + 22, cy = py + ph * 0.5f;
    DrawCircle((int)cx, (int)cy, 16.f, {28, 20, 12, 230});
    DrawCircle((int)cx, (int)cy, 14.f,
               isPlayer ? Color{80, 160, 220, 200} : Color{220, 80, 80, 200});
    // Avatar initials
    const char *initials = isPlayer ? "P" : "AI";
    int iw = MeasureText(initials, 11);
    DrawText(initials, (int)cx - iw/2, (int)(cy - 6), 11, {240, 240, 240, 255});
    // Portrait ring
    DrawCircleLines((int)cx, (int)cy, 16.f, borderCol);

    // ── HP label + value ──────────────────────────────────────────────────
    char hpBuf[16]; snprintf(hpBuf, 16, "%d", life);
    DrawText("HP", (int)(px + 44), (int)(py + 8), 9, {180, 155, 100, 190});
    DrawText(hpBuf, (int)(px + 60), (int)(py + 5), 16, {255, 222, 152, 255});

    // ── HP bar ────────────────────────────────────────────────────────────
    float hpFrac = fmaxf(0.0f, fminf(1.0f, (float)life / 20.0f));
    float barX = px + 44, barY = py + 28, barW = 144, barH = 9;
    // Track
    DrawRectangleRounded({barX, barY, barW, barH}, 0.6f, 4, {35, 18, 18, 220});
    // Fill — green → orange → red gradient based on HP fraction
    Color fillCol = hpFrac > 0.5f ? Color{72, (unsigned char)(202*hpFrac), 52, 245}
                                  : Color{(unsigned char)(255*(1-hpFrac)*2),
                                          (unsigned char)(140*hpFrac*2), 30, 245};
    if (hpFrac > 0.001f)
      DrawRectangleRounded({barX, barY, barW * hpFrac, barH}, 0.6f, 4, fillCol);
    // Bar shine overlay
    DrawRectangleGradientV((int)barX + 1, (int)barY + 1,
                            (int)(barW * hpFrac) - 2, 3,
                            {255, 255, 255, 55}, {0, 0, 0, 0});

    // ── Coins ─────────────────────────────────────────────────────────────
    DrawCircle((int)(px + 48), (int)(py + 43), 5, {200, 180, 40, 220});
    DrawCircleLines((int)(px + 48), (int)(py + 43), 5, {240, 215, 80, 200});
    char coinBuf[16]; snprintf(coinBuf, 16, "%d", coins);
    DrawText(coinBuf, (int)(px + 57), (int)(py + 38), 11, {222, 198, 55, 255});
  };

  // Player panel (bottom-left)
  DrawHUDPanel(8.f, (float)(SCREEN_H - 192), human.life, human.coins, true);
  // AI panel (top-left)
  DrawHUDPanel(8.f, 30.f, ai.life, ai.coins, false);

  // ── Turn banner flag (slides in on turn change) ───────────────────────────
  DrawTurnBanner(g_turnBannerTimer, g_turnBannerWho);

  // ── Phase action buttons ──────────────────────────────────────────────────
  // Phase action buttons (clickable)
  if (m.turn == 0 && m.phase == PHASE_DEVELOP) {
    DrawRectangleRounded({(float)(SCREEN_W - 187), (float)(SCREEN_H - 185),
                          177, 30}, 0.3f, 4, {40, 80, 40, 235});
    DrawRectangleRoundedLinesEx({(float)(SCREEN_W - 187), (float)(SCREEN_H - 185),
                                  177, 30}, 0.3f, 4, 1.5f, {100, 220, 100, 200});
    DrawText("▶ ACTIVATE PHASE", SCREEN_W - 182, SCREEN_H - 178,
             10, {160, 255, 160, 255});
  }
  if (m.turn == 0 && m.phase == PHASE_ACTIVATE) {
    DrawRectangleRounded({(float)(SCREEN_W - 187), (float)(SCREEN_H - 185),
                          177, 30}, 0.3f, 4, {90, 40, 40, 235});
    DrawRectangleRoundedLinesEx({(float)(SCREEN_W - 187), (float)(SCREEN_H - 185),
                                  177, 30}, 0.3f, 4, 1.5f, {220, 100, 100, 200});
    DrawText("■ END TURN", SCREEN_W - 182, SCREEN_H - 178,
             11, {255, 180, 180, 255});
    // Right-click hint
    DrawText("Right-click unit = DEFEND toggle", SCREEN_W - 182, SCREEN_H - 205,
             8, {180, 180, 255, 180});
    if (m.selectedFieldIdx >= 0) {
      DrawRectangleRounded({(float)(SCREEN_W - 187), (float)(SCREEN_H - 220),
                            177, 30}, 0.3f, 4, {122, 42, 42, 225});
      DrawRectangleRoundedLinesEx({(float)(SCREEN_W - 187), (float)(SCREEN_H - 220),
                                    177, 30}, 0.3f, 4, 1.5f, {220, 80, 80, 200});
      DrawText("⚔ ATTACK PLAYER", SCREEN_W - 182, SCREEN_H - 213,
               10, {255, 160, 160, 255});
    }
  }

  // ── Combat targeting arrow (Hearthstone-style Bezier) ────────────────────
  DrawCombatArrow();

  // ── AI "Thinking..." indicator ────────────────────────────────────────────
  DrawAIThinkingIndicator();

  // ── Arena combat burst particles ─────────────────────────────────────────
  DrawArenaBursts();

  // ── Card animations (highest Z — always on top) ───────────────────────────
  DrawCardAnims();

  // ── Message bar ───────────────────────────────────────────────────────────
  if (m.messageTimer > 0) {
    int mw = MeasureText(m.message, 14);
    float alpha = fminf(m.messageTimer, 1.0f) * 255.0f;
    DrawRectangleRounded(
        {SCREEN_W / 2.0f - mw / 2.0f - 15, SCREEN_H / 2.0f - 90,
         (float)mw + 30, 30},
        0.3f, 4,
        {20, 15, 10, (unsigned char)(alpha * 0.78f)});
    DrawText(m.message, SCREEN_W / 2 - mw / 2,
             (int)(SCREEN_H / 2.0f - 82), 14,
             {255, 232, 162, (unsigned char)alpha});
  }

  // ── Game Over overlay ─────────────────────────────────────────────────────
  if (m.phase == PHASE_GAME_OVER) {
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, {0, 0, 0, 162});
    const char *result = m.playerWon ? "VICTORY!" : "DEFEAT!";
    Color rc = m.playerWon ? Color{255, 222, 82, 255} : Color{255, 82, 82, 255};
    // Glow ring behind text
    float glow = sinf(g_time * 3.0f) * 0.5f + 0.5f;
    DrawCircle(SCREEN_W / 2, SCREEN_H / 2 - 28,
               (int)(88 + glow * 12),
               {(unsigned char)(rc.r / 5), (unsigned char)(rc.g / 5),
                (unsigned char)(rc.b / 5), (unsigned char)(58 + glow * 42)});
    int rw = MeasureText(result, 50);
    DrawText(result, SCREEN_W / 2 - rw / 2, SCREEN_H / 2 - 50, 50, rc);
    DrawText("Press ENTER to continue", SCREEN_W / 2 - 95,
             SCREEN_H / 2 + 22, 14, {202, 182, 142, 255});
  }

  // ── Floating damage / effect text ────────────────────────────────────────
  DrawFloatTexts();

  // ── Desert Wind & Dust atmospherics (shared with overworld system) ────────
  DrawVFX();

  // ── Hover tooltip: hand card ──────────────────────────────────────────────
  if (g_matchHoverHand >= 0 && g_matchHoverHand < human.handSize) {
    const CardDef &cd = GetCard(human.hand[g_matchHoverHand]);
    Rectangle hr = GetHandCardRect(g_matchHoverHand, human.handSize);
    float tipY = hr.y - 88;
    float tipX = hr.x - 28;
    if (tipX < 5) tipX = 5;
    if (tipX > SCREEN_W - 222) tipX = SCREEN_W - 222;
    DrawRectangleRounded({tipX, tipY, 215, 84}, 0.2f, 4, {24, 19, 14, 248});
    DrawRectangleRoundedLinesEx({tipX, tipY, 215, 84}, 0.2f, 4, 1.5f,
                                 {182, 152, 62, 255});
    DrawText(cd.name, (int)tipX + 6, (int)tipY + 5, 12, {255, 232, 162, 255});
    if (cd.effect[0])
      DrawText(cd.effect, (int)tipX + 6, (int)tipY + 22, 8,
               {202, 182, 142, 222});
    if (cd.isUnit) {
      char sb[52];
      snprintf(sb, 52, "ATK:%d  DEF:%d  Cost:%d", cd.atk, cd.def, cd.cost);
      DrawText(sb, (int)tipX + 6, (int)tipY + 56, 9, {222, 202, 162, 255});
      if (cd.keywords[0]) {
        char kb[52];
        snprintf(kb, 52, "Keywords: %s", cd.keywords);
        DrawText(kb, (int)tipX + 6, (int)tipY + 70, 8, {182, 222, 255, 222});
      }
    } else {
      char sb[18]; snprintf(sb, 18, "Cost: %d", cd.cost);
      DrawText(sb, (int)tipX + 6, (int)tipY + 56, 9, {222, 202, 162, 255});
    }
  }

  // ── Hover tooltip: player field unit ─────────────────────────────────────
  if (g_matchHoverField >= 0 && g_matchHoverField < human.fieldSize) {
    FieldUnit &fu = human.field[g_matchHoverField];
    const CardDef &cd = GetCard(fu.cardId);
    Rectangle fr = GetFieldScreenRect(0, g_matchHoverField, human.fieldSize,
                                       60, 45);
    float tipX = fr.x + fr.width + 5;
    float tipY = fr.y - 22;
    if (tipX > SCREEN_W - 204) tipX = fr.x - 207;
    DrawRectangleRounded({tipX, tipY, 202, 68}, 0.2f, 4, {24, 19, 14, 238});
    DrawRectangleRoundedLinesEx({tipX, tipY, 202, 68}, 0.2f, 4, 1.5f,
                                 {142, 122, 62, 255});
    DrawText(cd.name, (int)tipX + 5, (int)tipY + 5, 11, {255, 232, 162, 255});
    if (cd.effect[0])
      DrawText(cd.effect, (int)tipX + 5, (int)tipY + 20, 7,
               {202, 182, 142, 222});
    if (cd.keywords[0]) {
      char kb[34]; snprintf(kb, 34, "[%s]", cd.keywords);
      DrawText(kb, (int)tipX + 5, (int)tipY + 52, 8, {182, 222, 255, 222});
    }
  }

  // ── Graveyard gallery modal ──────────────────────────────────────────────
  if (g_graveModalOpen) {
    MatchPlayer &gmp = m.players[g_graveModalPlayer];
    // Dim the background
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, {0, 0, 0, 195});

    // Modal panel
    const float MW = 680.f, MH = 460.f;
    float MX = (SCREEN_W - MW) * 0.5f, MY = (SCREEN_H - MH) * 0.5f;
    DrawRectangleRounded({MX, MY, MW, MH}, 0.06f, 6, {16, 10, 22, 252});
    DrawRectangleRoundedLinesEx({MX, MY, MW, MH}, 0.06f, 6, 2.f,
                                 {130, 80, 180, 255});

    // Title bar
    const char *title = (g_graveModalPlayer == 0) ? "Your Graveyard" : "AI Graveyard";
    DrawRectangleRounded({MX, MY, MW, 32}, 0.06f, 4, {46, 22, 62, 255});
    int tw2 = MeasureText(title, 14);
    DrawText(title, (int)(MX + (MW - tw2) * 0.5f), (int)(MY + 8), 14,
             {200, 160, 255, 255});
    // Close hint
    DrawText("[ESC] close", (int)(MX + MW - 92), (int)(MY + 10), 9,
             {160, 130, 200, 200});

    // Card grid (sorted by: units first, then by cost ascending)
    static int sortedIds[MAX_GRAVE];
    int sortCount = gmp.graveSize;
    for (int i = 0; i < sortCount; i++) sortedIds[i] = gmp.grave[i];
    // Insertion sort: units before supports, then by cost
    for (int i = 1; i < sortCount; i++) {
      int key = sortedIds[i];
      int j = i - 1;
      const CardDef &kcd = GetCard(key);
      while (j >= 0) {
        const CardDef &jcd = GetCard(sortedIds[j]);
        bool jBefore = (jcd.isUnit > kcd.isUnit) ||
                       (jcd.isUnit == kcd.isUnit && jcd.cost <= kcd.cost);
        if (jBefore) break;
        sortedIds[j + 1] = sortedIds[j];
        j--;
      }
      sortedIds[j + 1] = key;
    }

    const float CW = 78.f, CH = 110.f, PAD = 10.f;
    int cols = (int)((MW - PAD * 2) / (CW + PAD));
    if (cols < 1) cols = 1;
    float gridX = MX + PAD;
    float gridTop = MY + 36.f;
    float gridH = MH - 40.f;
    // Clamp scroll
    int rows = (sortCount + cols - 1) / cols;
    float maxScroll = fmaxf(0.f, rows * (CH + PAD) - gridH + PAD);
    if (g_graveScrollY > maxScroll) g_graveScrollY = maxScroll;

    // Scissor to modal grid area
    BeginScissorMode((int)MX, (int)gridTop, (int)MW, (int)gridH);
    for (int i = 0; i < sortCount; i++) {
      int cid = sortedIds[i];
      const CardDef &cd2 = GetCard(cid);
      int col = i % cols, row = i / cols;
      float cx = gridX + col * (CW + PAD);
      float cy = gridTop + row * (CH + PAD) - g_graveScrollY;
      if (cy + CH < gridTop || cy > gridTop + gridH) continue; // out of view

      Rectangle cr = {cx, cy, CW, CH};
      // Card background
      DrawRectangleRounded(cr, 0.12f, 4, {28, 14, 36, 238});
      // Art texture
      if (cid > 0 && cid < 200 && g_cardTextures[cid].id != 0) {
        Rectangle src2 = {0, 0, (float)CARD_TEX_W, (float)CARD_TEX_H};
        DrawTexturePro(g_cardTextures[cid], src2, cr, {0,0}, 0.f,
                       {200, 180, 220, 220});
      }
      // Dark gradient at bottom for text legibility
      DrawRectangleGradientV((int)cx, (int)(cy + CH * 0.55f),
                             (int)CW, (int)(CH * 0.45f),
                             {0,0,0,0}, {0,0,0,200});
      // Name
      DrawText(cd2.name, (int)(cx + 3), (int)(cy + CH - 22), 7,
               {230, 210, 255, 255});
      // ATK/DEF or cost
      if (cd2.isUnit) {
        char sb2[16]; snprintf(sb2, 16, "%d/%d", cd2.atk, cd2.def);
        int sw = MeasureText(sb2, 8);
        DrawText(sb2, (int)(cx + CW - sw - 3), (int)(cy + CH - 12), 8,
                 {255, 200, 150, 255});
      } else {
        char sb2[8]; snprintf(sb2, 8, "c:%d", cd2.cost);
        int sw = MeasureText(sb2, 8);
        DrawText(sb2, (int)(cx + CW - sw - 3), (int)(cy + CH - 12), 8,
                 {180, 255, 200, 255});
      }
      // Border
      DrawRectangleRoundedLinesEx(cr, 0.12f, 4, 1.2f, {140, 90, 180, 200});
    }
    EndScissorMode();

    // Scroll indicator (right edge bar)
    if (maxScroll > 0.f) {
      float barH = gridH * (gridH / (rows * (CH + PAD)));
      if (barH < 18.f) barH = 18.f;
      float barY = gridTop + (g_graveScrollY / maxScroll) * (gridH - barH);
      DrawRectangleRounded({MX + MW - 8, barY, 5, barH}, 0.5f, 4,
                            {160, 120, 210, 200});
    }
  }
}

// ── Per-city shop stock (singles for sale) ───────────────────────────────────
static constexpr int SHOP_STOCK_SIZE = 20;
static int g_shopStock[5][SHOP_STOCK_SIZE]; // per city, card IDs
static bool g_shopStockGenerated[5] = {};

static void GenerateShopStock(int cityIdx) {
  if (cityIdx < 0 || cityIdx >= 5) return;
  if (g_shopStockGenerated[cityIdx]) return;
  g_shopStockGenerated[cityIdx] = true;
  // Each city biases toward certain card subtypes/costs
  // City 0=Aggro (low cost, high atk), 1=Control (high def, supports)
  // 2=Combo (demons, bugs), 3=Midrange (beasts, golems), 4=Tempo (mixed)
  int count = 0;
  int attempts = 0;
  while (count < SHOP_STOCK_SIZE && attempts < 200) {
    int idx = rand() % NUM_ALL_CARDS;
    const CardDef &cd = ALL_CARDS[idx];
    bool pick = false;
    switch (cityIdx) {
      case 0: pick = (cd.isUnit && cd.cost <= 2 && cd.atk >= 3); break; // Aggro
      case 1: pick = (!cd.isUnit || (cd.isUnit && cd.def >= 5)); break; // Control
      case 2: pick = (cd.subtype && (strstr(cd.subtype,"demon") || strstr(cd.subtype,"bug"))); break;
      case 3: pick = (cd.subtype && (strstr(cd.subtype,"beast") || strstr(cd.subtype,"golem"))); break;
      case 4: pick = (cd.cost >= 1 && cd.cost <= 3); break; // Tempo
    }
    if (!pick && (rand() % 5 == 0)) pick = true; // 20% random to fill
    if (pick) {
      // Avoid duplicates
      bool dup = false;
      for (int i = 0; i < count; i++)
        if (g_shopStock[cityIdx][i] == cd.id) { dup = true; break; }
      if (!dup) g_shopStock[cityIdx][count++] = cd.id;
    }
    attempts++;
  }
  // Fill remaining with random cards
  while (count < SHOP_STOCK_SIZE) {
    int id = ALL_CARDS[rand() % NUM_ALL_CARDS].id;
    bool dup = false;
    for (int i = 0; i < count; i++)
      if (g_shopStock[cityIdx][i] == id) { dup = true; break; }
    if (!dup) g_shopStock[cityIdx][count++] = id;
  }
}

static int g_shopSinglesScroll = 0;

// ── Pack opening helper (GDD-compliant slot rates) ──────────────────────────
static void OpenOnePack() {
  bool hasFairScale = g_inventory.HasFairScale();
  // Build rarity pools (lazy static)
  static int commonIds[200], rareIds[100], epicIds[100];
  static int nCommon = 0, nRare = 0, nEpic = 0;
  static bool poolsBuilt = false;
  if (!poolsBuilt) {
    for (int c = 0; c < NUM_ALL_CARDS; c++) {
      if (ALL_CARDS[c].isUnique)      epicIds[nEpic++] = ALL_CARDS[c].id;
      else if (ALL_CARDS[c].rarity==1) rareIds[nRare++] = ALL_CARDS[c].id;
      else                             commonIds[nCommon++] = ALL_CARDS[c].id;
    }
    poolsBuilt = true;
  }
  for (int i = 0; i < 10; i++) {
    int cardId;
    float roll = (float)rand() / (float)RAND_MAX;
    if (i < 9) {
      float epicThresh = hasFairScale ? 0.02f : 0.01f;
      float rareThresh = hasFairScale ? 0.10f : 0.05f;
      if (roll < epicThresh && nEpic > 0)
        cardId = epicIds[rand() % nEpic];
      else if (roll < rareThresh && nRare > 0)
        cardId = rareIds[rand() % nRare];
      else
        cardId = commonIds[rand() % nCommon];
    } else {
      float epicThresh = hasFairScale ? 0.10f : 0.05f;
      if (roll < epicThresh && nEpic > 0)
        cardId = epicIds[rand() % nEpic];
      else if (nRare > 0)
        cardId = rareIds[rand() % nRare];
      else
        cardId = commonIds[rand() % nCommon];
    }
    if (g_collectionSize < MAX_COLLECTION)
      g_collection[g_collectionSize++] = cardId;
  }
  g_market.OnPackOpened();
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
  int packPrice = (int)(10 * g_worldClock.GetShopMod() * g_inventory.GetPackMod() + 0.5f);
  Rectangle packBtn = {SCREEN_W / 2 - 100, 260, 200, 40};
  if (CheckCollisionPointRec(mouse, packBtn) &&
      IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
    if (g_playerCoins >= packPrice) {
      g_playerCoins -= packPrice;
      OpenOnePack();
    }
  }

  // Buy box button — 200 coins (with mods), 24 packs + 1 epic promo
  int boxPrice = (int)(200 * g_worldClock.GetShopMod() * g_inventory.GetBoxMod() + 0.5f);
  Rectangle boxBtn = {SCREEN_W / 2 - 100, 310, 200, 40};
  if (CheckCollisionPointRec(mouse, boxBtn) &&
      IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
    if (g_playerCoins >= boxPrice) {
      g_playerCoins -= boxPrice;
      for (int p = 0; p < 24; p++) OpenOnePack();
      // Bonus: 1 guaranteed epic promo card
      // Pick a random epic from the pool
      static int epicPromoIds[100];
      static int nEpicPromo = 0;
      static bool epicPromosBuilt = false;
      if (!epicPromosBuilt) {
        for (int c = 0; c < NUM_ALL_CARDS; c++)
          if (ALL_CARDS[c].isUnique) epicPromoIds[nEpicPromo++] = ALL_CARDS[c].id;
        epicPromosBuilt = true;
      }
      if (nEpicPromo > 0 && g_collectionSize < MAX_COLLECTION)
        g_collection[g_collectionSize++] = epicPromoIds[rand() % nEpicPromo];
    }
  }

  // Generate city shop stock on first visit
  if (g_currentShopCity >= 0 && g_currentShopCity < 5)
    GenerateShopStock(g_currentShopCity);

  // Buy singles from city shop stock (buttons in draw, clicks handled here)
  if (g_currentShopCity >= 0 && g_currentShopCity < 5) {
    float dayMod = g_worldClock.GetShopMod();
    float cardBuyMod = g_inventory.GetCardBuyMod();
    int startS = g_shopSinglesScroll;
    for (int s = startS; s < SHOP_STOCK_SIZE && s < startS + 5; s++) {
      int cid = g_shopStock[g_currentShopCity][s];
      if (cid <= 0) continue;
      int price = g_market.GetBuyPrice(cid, dayMod * cardBuyMod);
      Rectangle sBtn = {(float)(SCREEN_W - 220), (float)(140 + (s - startS) * 24), 210.f, 22.f};
      if (CheckCollisionPointRec(mouse, sBtn) &&
          IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        if (g_playerCoins >= price && g_collectionSize < MAX_COLLECTION) {
          g_playerCoins -= price;
          g_collection[g_collectionSize++] = cid;
          g_market.OnCardBought(cid);
        }
      }
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

  // Singles scroll (Page Up/Down or left/right arrows)
  if (IsKeyPressed(KEY_PAGE_DOWN) || IsKeyPressed(KEY_RIGHT))
    g_shopSinglesScroll = (g_shopSinglesScroll + 5 < SHOP_STOCK_SIZE)
                          ? g_shopSinglesScroll + 5 : g_shopSinglesScroll;
  if (IsKeyPressed(KEY_PAGE_UP) || IsKeyPressed(KEY_LEFT))
    g_shopSinglesScroll = (g_shopSinglesScroll >= 5) ? g_shopSinglesScroll - 5 : 0;

  // Exit shop
  if (IsKeyPressed(KEY_ESCAPE)) {
    g_shopSinglesScroll = 0;
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
  int packPrice = (int)(10 * g_worldClock.GetShopMod() * g_inventory.GetPackMod() + 0.5f);
  Color packCol = (g_playerCoins >= packPrice) ? Color{80, 80, 140, 255}
                                               : Color{50, 50, 60, 255};
  DrawRectangleRounded({(float)SCREEN_W / 2 - 100, 260, 200, 40}, 0.3f, 4,
                       packCol);
  snprintf(buf, 128, "Buy Card Pack (%d coins)", packPrice);
  DrawText(buf, SCREEN_W / 2 - 85, 272, 12, {255, 255, 200, 255});

  // Box button (24 packs + 1 epic promo)
  int boxPrice = (int)(200 * g_worldClock.GetShopMod() * g_inventory.GetBoxMod() + 0.5f);
  Color boxCol = (g_playerCoins >= boxPrice) ? Color{120, 80, 40, 255}
                                             : Color{60, 45, 25, 255};
  DrawRectangleRounded({(float)SCREEN_W / 2 - 100, 310, 200, 40}, 0.3f, 4, boxCol);
  snprintf(buf, 128, "Buy Box (%d coins)", boxPrice);
  DrawText(buf, SCREEN_W / 2 - 85, 318, 12, {255, 230, 160, 255});
  DrawText("24 packs + 1 Epic promo", SCREEN_W / 2 - 75, 334, 8, {200, 180, 120, 180});

  // Time of day indicator
  snprintf(buf, 128, "Time: %s (price mod: %.0f%%)", g_worldClock.GetName(),
           (g_worldClock.GetShopMod() - 1.0f) * 100);
  DrawText(buf, SCREEN_W / 2 - 100, 95, 10, {180, 160, 120, 200});

  // Per-city singles for sale (right panel)
  if (g_currentShopCity >= 0 && g_currentShopCity < 5) {
    static const char *cityNames[] = {"Zahav","Ma'ayan","Avak","Gan","Sela"};
    snprintf(buf, 128, "%s Singles:", cityNames[g_currentShopCity]);
    DrawText(buf, SCREEN_W - 225, 120, 12, {255, 200, 100, 255});
    float dayMod2 = g_worldClock.GetShopMod();
    float cardBuyMod = g_inventory.GetCardBuyMod();
    int startS = g_shopSinglesScroll;
    for (int s = startS; s < SHOP_STOCK_SIZE && s < startS + 5; s++) {
      int cid = g_shopStock[g_currentShopCity][s];
      if (cid <= 0) continue;
      const CardDef &scd = GetCard(cid);
      int price = g_market.GetBuyPrice(cid, dayMod2 * cardBuyMod);
      int sy = 140 + (s - startS) * 24;
      Color btnCol = (g_playerCoins >= price) ? Color{60, 80, 60, 255}
                                               : Color{40, 40, 40, 255};
      DrawRectangleRounded({(float)(SCREEN_W - 220), (float)sy, 210.f, 22.f},
                           0.2f, 4, btnCol);
      snprintf(buf, 128, "%s - %d coins", scd.name, price);
      DrawText(buf, SCREEN_W - 215, sy + 4, 9, {220, 210, 180, 255});
    }
    // Scroll arrows
    if (g_shopSinglesScroll > 0)
      DrawText("[UP]", SCREEN_W - 160, 130, 8, {180, 180, 120, 180});
    if (g_shopSinglesScroll + 5 < SHOP_STOCK_SIZE)
      DrawText("[DOWN]", SCREEN_W - 160, 140 + 5 * 24, 8, {180, 180, 120, 180});
  }

  // Collection display with sell prices
  DrawText("Your Collection (click [S] next to card to sell):", 30, 370, 14,
           {220, 200, 160, 255});
  int startIdx = g_shopScroll * 8;
  int y = 390;
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
  } else if (g_targetNPC >= 0 && g_npcs[g_targetNPC].nomadic) {
    // ── Nomadic merchant menu ──────────────────────────────────────────
    int itemId = g_npcs[g_targetNPC].nomadicItem;
    const char *itemName = (itemId >= 0 && !g_inventory.Has((ItemId)itemId))
                           ? g_inventory.items[itemId].name : nullptr;
    if (itemName) {
      char offerBuf[128];
      snprintf(offerBuf, sizeof(offerBuf),
               "I carry a rare item on my travels:\n  %s\nA small donation (5-20 coins)?", itemName);
      DrawText(offerBuf, msgBoxX + 18, msgBoxY + 14, 13, {230, 220, 190, 255});
    } else {
      DrawText("I have given away all my wares.\nPerhaps our paths cross again.",
               msgBoxX + 18, msgBoxY + 14, 13, {180, 170, 150, 255});
    }
    const char *opts[] = {"Accept", "Decline"};
    for (int i = 0; i < 2; i++) {
      bool sel = (i == g_dialogSelection);
      Color col = sel ? Color{255, 220, 80, 255} : Color{210, 200, 170, 255};
      char buf[32];
      snprintf(buf, sizeof(buf), "%s%s", sel ? "> " : "  ", opts[i]);
      DrawText(buf, msgBoxX + 18, msgBoxY + msgBoxH - 55 + i * 20, 16, col);
    }
    DrawText("[W/S] Select  [Enter/E] Confirm  [Esc] Close",
             msgBoxX + 18, msgBoxY + msgBoxH - 22, 10, {160, 140, 100, 160});
  } else {
    // ── Menu phase: Talk / Trade / Duel / Close ────────────────────────
    bool isTournamentMaster = (g_targetNPC >= 0 &&
        g_npcs[g_targetNPC].role == NPC_ROLE_TOURNAMENT);
    const char *duelLabel = isTournamentMaster ? "Tournament" : "Duel";
    const char *options[] = {"Talk", "Trade", duelLabel, "Close"};
    for (int i = 0; i < 4; i++) {
      bool sel = (i == g_dialogSelection);
      Color col = sel ? Color{255, 220, 80, 255} : Color{210, 200, 170, 255};
      const char *arrow = sel ? "> " : "  ";
      char buf[64];
      snprintf(buf, sizeof(buf), "%s%s", arrow, options[i]);
      DrawText(buf, msgBoxX + 18, msgBoxY + 14 + i * 20, 16, col);
    }
    // Tournament master: show city round progress in dialog
    if (isTournamentMaster) {
      int ci = g_npcs[g_targetNPC].cityIndex;
      bool badgeEarned = g_tournament.cityChampionDefeated[ci];
      char statusBuf[80];
      if (badgeEarned) {
        snprintf(statusBuf, sizeof(statusBuf), "City Badge: EARNED");
        DrawText(statusBuf, msgBoxX + msgBoxW - 180, msgBoxY + 14, 10, {100,255,100,200});
      } else {
        snprintf(statusBuf, sizeof(statusBuf), "Round %d/3",
                 (g_tournament.currentCity == ci) ? g_tournament.roundsWon : 0);
        DrawText(statusBuf, msgBoxX + msgBoxW - 120, msgBoxY + 14, 10, {220,200,100,200});
      }
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
    // In talk mode, any confirm/escape closes the dialog
    if (confirm || IsKeyPressed(KEY_ESCAPE) || g_padBPressed) {
      // For nomadic NPCs the talk phase is a one-shot farewell — close entirely
      if (g_targetNPC >= 0 && g_npcs[g_targetNPC].nomadic) {
        g_npcDialogOpen = false;
        g_dialogPhase   = DIALOG_MENU;
        g_dialogText    = nullptr;
      } else {
        g_dialogPhase = DIALOG_MENU;
        g_dialogText  = nullptr;
      }
    }
    return;
  }

  // ── Menu navigation ────────────────────────────────────────────────────
  int maxSel = (g_targetNPC >= 0 && g_npcs[g_targetNPC].nomadic) ? 1 : 3;
  if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) {
    g_dialogSelection--;
    if (g_dialogSelection < 0) g_dialogSelection = maxSel;
  }
  if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) {
    g_dialogSelection++;
    if (g_dialogSelection > maxSel) g_dialogSelection = 0;
  }

  // ── Nomadic NPC special interaction ─────────────────────────────────────
  if (g_targetNPC >= 0 && g_npcs[g_targetNPC].nomadic) {
    int itemId = g_npcs[g_targetNPC].nomadicItem;
    if (confirm) {
      if (g_dialogSelection == 0) { // Accept donation
        if (itemId >= 0 && !g_inventory.Has((ItemId)itemId)) {
          int cost = GetRandomValue(5, 20);
          g_playerCoins = (g_playerCoins > cost) ? g_playerCoins - cost : 0;
          g_inventory.Give((ItemId)itemId);
          g_npcs[g_targetNPC].nomadicItem = -1; // item taken
          g_dialogText = "A small fortune for a great treasure.\nSafe travels, friend!";
          g_dialogPhase = DIALOG_TALK;
        } else if (itemId < 0 || g_inventory.Has((ItemId)itemId)) {
          g_dialogText = "I have already given you all I have.\nPerhaps we'll meet again!";
          g_dialogPhase = DIALOG_TALK;
        }
      } else { // Decline or Close
        g_npcDialogOpen = false;
        g_dialogPhase = DIALOG_MENU;
      }
    }
    return;
  }

  if (confirm) {
    PlaySfx(SFX_CONFIRM);
    switch (g_dialogSelection) {
    case 0: { // Talk — show random card-game lore
      int line = GetRandomValue(0, 2);
      int idx = (g_targetNPC >= 0 && g_targetNPC < 4) ? g_targetNPC : 0;
      g_dialogText = g_npcLore[idx][line];
      g_dialogPhase = DIALOG_TALK;
      break;
    }
    case 1: // Trade
      if (g_targetNPC >= 0 && (g_targetNPC == 2 ||
          g_npcs[g_targetNPC].role == NPC_ROLE_SHOP)) {
        if (g_worldClock.IsNight()) {
          g_dialogText = "The shop is closed at night.\nCome back in the morning!";
          g_dialogPhase = DIALOG_TALK;
        } else {
          g_currentShopCity = g_npcs[g_targetNPC].cityIndex;
          // First-visit: give 2 free packs
          if (g_currentShopCity >= 0 && g_currentShopCity < 5 &&
              !g_shopCityVisited[g_currentShopCity]) {
            g_shopCityVisited[g_currentShopCity] = true;
            OpenOnePack();
            OpenOnePack();
          }
          g_scene = SCENE_SHOP;
          g_npcDialogOpen = false;
          g_dialogPhase = DIALOG_MENU;
        }
      } else {
        g_dialogText = "I have nothing to trade right now.\nVisit the city bazaar to shop!";
        g_dialogPhase = DIALOG_TALK;
      }
      break;
    case 2: // Duel / Tournament
      if (!g_hasStarterDeck) {
        g_dialogText = "You need a deck first! Visit the bazaar\nand pick up a starter pack.";
        g_dialogPhase = DIALOG_TALK;
      } else if (g_playerDeckSize < 20) {
        g_dialogText = "Your deck needs at least 20 cards.\nOpen some packs at the bazaar!";
        g_dialogPhase = DIALOG_TALK;
      } else if (g_targetNPC >= 0 &&
                 g_npcs[g_targetNPC].role == NPC_ROLE_TOURNAMENT) {
        if (!g_worldClock.IsMorning()) {
          g_dialogText = g_worldClock.IsNight()
            ? "Tournaments are closed at night.\nReturn in the morning!"
            : "Tournaments only run in the morning.\nCome back tomorrow!";
          g_dialogPhase = DIALOG_TALK;
        } else {
          // Tournament round
          int cityIdx = g_npcs[g_targetNPC].cityIndex;
          g_tournament.currentCity = cityIdx;
          g_tournamentMode = true;
          g_playerCoins -= g_tournament.GetEntryFee(); // deduct entry fee
          if (g_playerCoins < 0) g_playerCoins = 0;
          StartMatch(g_targetNPC);
          g_scene = SCENE_MATCH;
          g_npcDialogOpen = false;
          g_dialogPhase = DIALOG_MENU;
        }
      } else {
        StartMatch(g_targetNPC);
        g_scene = SCENE_MATCH;
        g_npcDialogOpen = false;
        g_dialogPhase = DIALOG_MENU;
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

        // Draw card with full frame layout
        if (cid > 0 && cid <= NUM_ALL_CARDS) {
          CardView cv; cv.Init(cid);
          Rectangle dst = {(float)cx2, (float)cy2, (float)cardW, (float)cardH};
          DrawCardView(dst, cv, cid);
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
      // Large card preview — full frame layout
      {
        Rectangle dst = {(float)(detX + detW / 2 - 72), (float)(gridY + 8), 144, 192};
        CardView pv; pv.Init(g_selectedCardId);
        DrawCardView(dst, pv, g_selectedCardId);
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
        if (CanAddToDeck(g_selectedCardId)) {
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
        if (CanAddToDeck(g_cardCopies[i].cardId)) {
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
  float cachedY; // cached GetDuneHeight — rocks don't move
  Model model;
};
static RockInstance g_rockInstances[MAX_ROCKS];
static int g_numRocks = 0;
// ── Vegetation instances (Northern Wastes cacti, agave, dry trees) ──────────
static constexpr int MAX_VEGETATION = 80;
enum VegType { VEG_SAGUARO = 0, VEG_PRICKLY_PEAR, VEG_AGAVE, VEG_DRY_TREE };
struct VegetationInstance {
  float x, z;
  float rotY;
  float scale;
  VegType type;
  float colliderRadius; // for sphere-slide collision
  float cachedY; // cached GetDuneHeight — vegetation doesn't move
};
static VegetationInstance g_vegInstances[MAX_VEGETATION];
static int g_numVeg = 0;

// Northern Wastes point-of-interest: ancient obelisk at north edge
static constexpr float OBELISK_X = 100.0f;
static constexpr float OBELISK_Z = 7.5f;
static float g_obeliskY = 0.0f; // cached at init


static void InitProceduralRocks() {
  g_numRocks = 0;
  Color rockCols[] = {{165, 140, 110, 255},
                      {180, 155, 120, 255},
                      {145, 125, 95, 255},
                      {190, 165, 130, 255}};
  for (int i = 0; i < MAX_ROCKS && g_numRocks < MAX_ROCKS; i++) {
    int rx = GetRandomValue(2, MAP_W - 3);
    int ry = GetRandomValue(2, MAP_H - 3);
    RockInstance &ri = g_rockInstances[g_numRocks++];
    ri.x = (float)rx;
    ri.z = (float)ry;
    ri.rotY = (float)GetRandomValue(0, 360);
    ri.scale = 0.3f + (float)GetRandomValue(0, 40) * 0.01f;
    ri.cachedY = GetDuneHeight(ri.x, ri.z);
    ri.model = GenerateProceduralRock(1.0f, rockCols[i % 4]);
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// §2  DESERT TENTS — AABB 4×4×4 Cube Meshes with 1×1 Door Void
// ═══════════════════════════════════════════════════════════════════════════════

struct TentInstance {
  int gx, gy;           // grid origin (top-left corner)
  float worldX, worldZ; // center world position
  float tentHS;         // half-size (2.0=normal 4×4, 3.0=large 6×6)
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

static constexpr int MAX_TENTS = 30;
static TentInstance g_tentInstances[MAX_TENTS];
static int g_numTents = 0;

static void PlaceTentSized(int gx, int gy, float hs, Color wall, Color roof,
                           const char *sign, Scene interior) {
  if (g_numTents >= MAX_TENTS)
    return;
  int sizeT = (int)(hs * 2.0f + 0.5f); // grid tiles = 2*hs
  TentInstance &t = g_tentInstances[g_numTents++];
  t.gx = gx;
  t.gy = gy;
  t.tentHS = hs;
  t.wallCol = wall;
  t.roofCol = roof;
  t.signText = sign;
  t.interiorScene = interior;
  t.worldX = (float)gx + hs;
  t.worldZ = (float)gy + hs;
  t.doorGX = gx + (int)hs;
  t.doorGY = gy + sizeT;
  // Clear interior tiles
  for (int dy = 1; dy < sizeT - 1; dy++)
    for (int ddx = 1; ddx < sizeT - 1; ddx++) {
      int tx = gx + ddx, ty = gy + dy;
      if (tx >= 0 && tx < MAP_W && ty >= 0 && ty < MAP_H)
        g_collision[ty][tx] = 0;
    }
  for (int dc = -1; dc <= 0; dc++) {
    int doorCol = t.doorGX + dc;
    int southEdge = gy + sizeT - 1;
    if (doorCol >= 0 && doorCol < MAP_W && southEdge >= 0 && southEdge < MAP_H)
      g_collision[southEdge][doorCol] = 0;
  }
  for (int extra = 0; extra <= 1; extra++) {
    int ty = t.doorGY + extra;
    for (int dc = -1; dc <= 0; dc++) {
      int tx = t.doorGX + dc;
      if (tx >= 0 && tx < MAP_W && ty >= 0 && ty < MAP_H)
        g_collision[ty][tx] = 0;
    }
  }
  t.wallAABB.min = {t.worldX - hs, 0.0f, t.worldZ - hs};
  t.wallAABB.max = {t.worldX + hs, 4.0f, t.worldZ + hs};
  float ddx = (float)t.doorGX + 0.5f, ddz = (float)t.doorGY + 0.5f;
  t.doorAABB.min = {ddx - 1.0f, 0.0f, ddz - 0.6f};
  t.doorAABB.max = {ddx + 1.0f, 2.0f, ddz + 0.6f};
  t.interiorAABB.min = {t.worldX - hs + 0.4f, 0.0f, t.worldZ - hs + 0.4f};
  t.interiorAABB.max = {t.worldX + hs - 0.4f, 4.0f, t.worldZ + hs + 0.8f};
  t.roofVisible = true;
  t.frontWallVisible = true;
  t.roofAlpha = 1.0f;
  t.frontAlpha = 1.0f;
}

static void PlaceTent(int gx, int gy, Color wall, Color roof, const char *sign,
                      Scene interior) {
  if (g_numTents >= MAX_TENTS)
    return;
  TentInstance &t = g_tentInstances[g_numTents++];
  t.gx = gx;
  t.gy = gy;
  t.tentHS = 2.0f;
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
  // Collision is mesh-based (wallAABB below) — no tile marks needed.
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
  // AABB: door trigger (2.0 wide × 1.2 deep, centered on door tile)
  float ddx = (float)t.doorGX + 0.5f, ddz = (float)t.doorGY + 0.5f;
  t.doorAABB.min = {ddx - 1.0f, 0.0f, ddz - 0.6f};
  t.doorAABB.max = {ddx + 1.0f, 2.0f, ddz + 0.6f};
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
  float hs = t.tentHS;   // half-size on XZ (2.0=normal, 3.0=large)
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
  Color woodCol   = {110, 82,  54, 255};
  Color woodDk    = { 82, 60,  38, 255};
  Color fabricCol = {124, 92,  58, 255};
  Color goldCol   = {210,168,  45, 255};
  Color redFab    = {160, 40,  40, 255};
  switch (tentIdx) {
  case 0: // Elder's Tent — low table + cushions + shelf
    DrawCube({cx, 0.22f, cz - 0.6f}, 1.6f, 0.08f, 0.9f, woodCol);
    DrawCube({cx - 0.6f, 0.20f, cz - 0.6f}, 0.08f, 0.22f, 0.08f, woodDk);
    DrawCube({cx + 0.6f, 0.20f, cz - 0.6f}, 0.08f, 0.22f, 0.08f, woodDk);
    DrawCube({cx - 1.4f, 1.0f, cz - 1.5f}, 0.30f, 2.0f, 0.70f, woodDk);
    DrawCube({cx - 0.8f, 0.10f, cz + 0.4f}, 0.55f, 0.10f, 0.55f, {180,130,80,255}); // cushion
    DrawCube({cx + 0.8f, 0.10f, cz + 0.4f}, 0.55f, 0.10f, 0.55f, {180,130,80,255});
    break;
  case 1: // Card Shop — long display counter + card rack + crates
    DrawCube({cx,       0.50f, cz - 0.8f}, 3.8f, 0.12f, 0.9f, woodCol);      // counter top
    DrawCube({cx - 1.7f,0.25f, cz - 0.8f}, 0.10f, 0.50f, 0.10f, woodDk);    // legs
    DrawCube({cx + 1.7f,0.25f, cz - 0.8f}, 0.10f, 0.50f, 0.10f, woodDk);
    DrawCube({cx,       0.25f, cz - 0.8f}, 3.8f, 0.50f, 0.08f, woodDk);      // front panel
    // Card display stands on counter
    for (int i = -2; i <= 2; i++) {
      DrawCube({cx + i*0.7f, 0.65f, cz - 0.85f}, 0.30f, 0.22f, 0.04f, {50,80,140,255});
    }
    // Shelving unit on back wall
    DrawCube({cx,       1.20f, cz - 1.65f}, 4.0f, 0.08f, 0.30f, woodDk);
    DrawCube({cx,       2.00f, cz - 1.65f}, 4.0f, 0.08f, 0.30f, woodDk);
    DrawCube({cx - 1.7f,1.60f, cz - 1.65f}, 0.10f, 0.88f, 0.30f, woodDk);   // side posts
    DrawCube({cx + 1.7f,1.60f, cz - 1.65f}, 0.10f, 0.88f, 0.30f, woodDk);
    // Crates at side
    DrawCube({cx - 1.4f, 0.30f, cz + 0.8f}, 0.70f, 0.60f, 0.70f, woodCol);
    DrawCube({cx - 1.4f, 0.75f, cz + 0.8f}, 0.65f, 0.55f, 0.65f, woodCol);
    break;
  case 2: // Healer's Tent — herb table + cots
    DrawCube({cx, 0.45f, cz - 1.0f}, 2.2f, 0.10f, 0.8f, woodCol);
    DrawCube({cx - 0.9f, 0.22f, cz - 1.0f}, 0.08f, 0.44f, 0.08f, woodDk);
    DrawCube({cx + 0.9f, 0.22f, cz - 1.0f}, 0.08f, 0.44f, 0.08f, woodDk);
    DrawCube({cx - 1.2f, 0.18f, cz + 0.5f}, 0.7f, 0.18f, 1.4f, {160,190,160,255}); // cot
    DrawCube({cx + 1.2f, 0.18f, cz + 0.5f}, 0.7f, 0.18f, 1.4f, {160,190,160,255});
    break;
  case 3: // Rest House — beds + firepit
    DrawCube({cx - 1.1f, 0.20f, cz - 0.5f}, 0.9f, 0.22f, 1.6f, {140,105,70,255});
    DrawCube({cx + 1.1f, 0.20f, cz - 0.5f}, 0.9f, 0.22f, 1.6f, {140,105,70,255});
    DrawCylinder({cx, 0.06f, cz + 0.8f}, 0.35f, 0.35f, 0.12f, 8, {80,70,60,255}); // firepit ring
    DrawSphere({cx, 0.28f, cz + 0.8f}, 0.10f, {255,160,30,255});                  // flame
    break;
  case 4: // House of Glory — central duel arena + benches + trophy shelf
    // Arena mat (large red octagon approximated by disc)
    DrawCylinder({cx, 0.04f, cz}, 2.2f, 2.2f, 0.04f, 8, {140,35,35,200});
    DrawCylinder({cx, 0.06f, cz}, 1.8f, 1.8f, 0.03f, 8, {170,45,45,200});
    // Benches along walls
    DrawCube({cx - 2.2f, 0.30f, cz},       0.45f, 0.30f, 3.0f, woodCol);
    DrawCube({cx + 2.2f, 0.30f, cz},       0.45f, 0.30f, 3.0f, woodCol);
    DrawCube({cx,        0.30f, cz - 2.2f},3.0f,  0.30f, 0.45f, woodCol);
    // Trophy shelf on back wall
    DrawCube({cx,        1.50f, cz - 2.5f},3.5f, 0.12f, 0.35f, woodDk);
    DrawCube({cx,        2.30f, cz - 2.5f},3.5f, 0.12f, 0.35f, woodDk);
    // Trophy cups on shelf
    for (int i = -2; i <= 2; i++) {
      DrawCylinder({cx + i*0.7f, 1.68f, cz - 2.52f}, 0.10f, 0.07f, 0.28f, 6, goldCol);
      DrawCylinder({cx + i*0.7f, 1.96f, cz - 2.52f}, 0.14f, 0.10f, 0.10f, 6, goldCol);
    }
    // Corner banners (flagpoles)
    for (int i = 0; i < 4; i++) {
      float ba = i*(PI*0.5f) + PI*0.25f;
      float bx = cx + cosf(ba)*2.3f, bz = cz + sinf(ba)*2.3f;
      DrawCylinder({bx, 0.0f, bz}, 0.06f, 0.05f, 3.2f, 6, woodDk);
      DrawCube({bx, 2.8f, bz}, 0.07f, 0.9f, 0.5f, redFab);
    }
    break;
  default: // distant/other tents — simple table
    DrawCube({cx, 0.40f, cz - 0.8f}, 1.6f, 0.08f, 0.9f, woodCol);
    DrawCube({cx - 0.6f, 0.20f, cz - 0.8f}, 0.08f, 0.40f, 0.08f, woodDk);
    DrawCube({cx + 0.6f, 0.20f, cz - 0.8f}, 0.08f, 0.40f, 0.08f, woodDk);
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
      float halfH2 = (float)(MAP_H / 2);
      float northP2 = Clamp((halfH2 - g_player.posZ) / halfH2, 0.0f, 1.0f);
      float southP2 = Clamp((g_player.posZ - halfH2) / halfH2, 0.0f, 1.0f);
      float alphaMult = 1.0f + northP2 * 1.6f + southP2 * 2.8f;
      p.alpha = (0.12f + (float)GetRandomValue(0, 8) * 0.01f) * alphaMult;
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
  // Dust density: higher at northern + southern extremes
  float halfH = (float)(MAP_H / 2);
  float northP = Clamp((halfH - g_player.posZ) / halfH, 0.0f, 1.0f);
  float southP = Clamp((g_player.posZ - halfH) / halfH, 0.0f, 1.0f);
  float dustMult = 1.0f + northP * 1.8f + southP * 3.2f; // sandstorm stronger in south
  float gustInterval = (8.5f + (float)GetRandomValue(0, 400) * 0.01f) / dustMult;
  g_gustTimer += dt;
  if (g_gustTimer >= g_nextGust) {
    g_gustTimer = 0.0f;
    g_nextGust = gustInterval;
    int extraGusts = (int)dustMult;
    for (int eg = 0; eg < extraGusts; eg++) SpawnWindGust();
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

// Sun direction — updated each frame from WorldClock for moving shadows
static Vector3 g_sunDir = {0.65f, -0.85f, 0.20f}; // default: morning east

// Recompute g_sunDir from time of day (call once per frame before drawing shadows)
static void UpdateSunDir() {
  float phase = g_worldClock.GetPhaseNorm();
  float dayT  = (phase < 0.667f) ? (phase / 0.667f) : 1.0f; // 0-1 day arc
  float az    = dayT * PI;          // 0=east morning, PI=west evening
  float elev  = sinf(dayT * PI);    // 0 at horizon, 1 at midday
  g_sunDir.x  =  cosf(az) * 0.65f; // +x=east shadows, -x=west shadows
  g_sunDir.y  = -0.70f - elev * 0.20f;
  g_sunDir.z  =  0.35f - elev * 0.28f; // shorter z component at high noon
}

// Project shadow as a dark stretched ellipse on ground plane
static void DrawSoftShadow(Vector3 objPos, float radius, float height) {
  // Shadow offset from sun direction projected onto ground
  float shadowX = objPos.x - g_sunDir.x * height * 0.6f;
  float shadowZ = objPos.z - g_sunDir.z * height * 0.6f;
  // Simple 2-layer shadow (outer soft + inner core) — no PCF sampling
  float gy = objPos.y + 0.02f;
  DrawCylinder({shadowX, gy, shadowZ}, radius * 1.2f, radius * 1.2f, 0.01f, 8,
               {40, 24, 6, 18});
  DrawCylinder({shadowX, gy + 0.005f, shadowZ}, radius, radius, 0.01f, 8,
               {32, 18, 4, 36});
}


// ─── Vegetation draw helpers (matte vinyl low-poly style) ────────────────────
static void DrawSaguaro(Vector3 base, float sc) {
  Color trunk  = {58, 92, 46, 255};
  Color trunkL = {72, 112, 56, 255};
  float h = 3.2f * sc;
  float r = 0.22f * sc;
  // Main trunk
  DrawCylinder(base, r, r * 0.85f, h, 7, trunk);
  DrawCylinderWires(base, r, r * 0.85f, h, 7, trunkL);
  // Left arm (branches up at 55% height)
  float ay = base.y + h * 0.55f;
  Vector3 armLBase = {base.x - r * 1.6f, ay, base.z};
  Vector3 armLTip  = {base.x - 0.7f * sc, ay + 0.9f * sc, base.z};
  DrawCylinder(armLBase, r * 0.72f, r * 0.6f, 0.55f * sc, 6, trunk);
  DrawCylinder(armLTip,  r * 0.6f,  r * 0.5f, 0.8f * sc, 6, trunk);
  // Right arm (branches up at 40% height)
  Vector3 armRBase = {base.x + r * 1.6f, base.y + h * 0.40f, base.z};
  Vector3 armRTip  = {base.x + 0.65f * sc, base.y + h * 0.40f + 0.8f * sc, base.z};
  DrawCylinder(armRBase, r * 0.72f, r * 0.60f, 0.50f * sc, 6, trunk);
  DrawCylinder(armRTip,  r * 0.60f, r * 0.50f, 0.75f * sc, 6, trunk);
  // Top cap
  Vector3 top = {base.x, base.y + h, base.z};
  DrawSphere(top, r * 0.9f, trunk);
}

static void DrawPricklyPear(Vector3 base, float sc) {
  Color padCol  = {65, 108, 55, 255};
  Color padEdge = {80, 132, 68, 255};
  float r = 0.30f * sc;
  // Three flattened pads stacked
  Vector3 p0 = {base.x, base.y + 0.05f, base.z};
  Vector3 p1 = {base.x + 0.18f * sc, base.y + 0.35f * sc, base.z + 0.05f * sc};
  Vector3 p2 = {base.x - 0.08f * sc, base.y + 0.60f * sc, base.z - 0.05f * sc};
  DrawCylinder(p0, r * 1.2f, r * 1.1f, 0.22f * sc, 8, padCol);
  DrawCylinderWires(p0, r * 1.2f, r * 1.1f, 0.22f * sc, 8, padEdge);
  DrawCylinder(p1, r * 0.9f, r * 0.8f, 0.20f * sc, 8, padCol);
  DrawCylinderWires(p1, r * 0.9f, r * 0.8f, 0.20f * sc, 8, padEdge);
  DrawCylinder(p2, r * 0.7f, r * 0.65f, 0.18f * sc, 8, padCol);
}

static void DrawAgave(Vector3 base, float sc) {
  Color leafCol = {78, 118, 68, 255};
  Color tipCol  = {190, 210, 155, 255};
  int leafCount = 8;
  float leafLen = 0.95f * sc;
  for (int i = 0; i < leafCount; i++) {
    float a = (float)i * (6.2832f / (float)leafCount);
    float tilt = 0.28f; // slight droop downward
    Vector3 tip = {base.x + cosf(a) * leafLen,
                   base.y + 0.12f * sc - sinf(tilt) * leafLen,
                   base.z + sinf(a) * leafLen};
    // Leaf as narrow cylinder tapering to a point
    DrawCylinder(base, 0.06f * sc, 0.01f * sc, leafLen, 5, leafCol);
    DrawSphere(tip, 0.045f * sc, tipCol);
    // Rotate each leaf by rotating base — approximate with offset cylinders
    (void)tip;
  }
  // Center rosette
  DrawCylinder(base, 0.10f * sc, 0.08f * sc, 0.18f * sc, 7, leafCol);
}

static void DrawDryTree(Vector3 base, float sc) {
  Color bark  = {82, 62, 42, 255};
  Color barkL = {100, 78, 52, 255};
  float h = 2.4f * sc;
  float r = 0.14f * sc;
  // Main trunk (twisted: slight tilt in X)
  DrawCylinder(base, r, r * 0.7f, h, 6, bark);
  DrawCylinderWires(base, r, r * 0.7f, h, 6, barkL);
  // 3 dead branches
  float bh = base.y + h * 0.65f;
  Vector3 b0s = {base.x, bh, base.z};
  Vector3 b1s = {base.x, base.y + h * 0.80f, base.z};
  Vector3 b2s = {base.x, base.y + h * 0.50f, base.z};
  DrawCylinder(b0s, r * 0.55f, r * 0.25f, 0.70f * sc, 5, bark);
  DrawCylinder(b1s, r * 0.45f, r * 0.20f, 0.55f * sc, 5, bark);
  DrawCylinder(b2s, r * 0.40f, r * 0.18f, 0.45f * sc, 5, bark);
  // Stubs
  Vector3 top = {base.x, base.y + h, base.z};
  DrawSphere(top, r * 0.6f, bark);
}

static void DrawNorthernObelisk() {
  Vector3 base = {OBELISK_X, g_obeliskY, OBELISK_Z};
  Color stone  = {142, 128, 105, 255};
  Color glow   = {200, 170,  80, 255};
  float pulse  = sinf(g_time * 1.8f) * 0.5f + 0.5f;
  // Base plinth
  DrawCube({base.x, base.y + 0.15f, base.z}, 1.2f, 0.30f, 1.2f, stone);
  // Shaft
  DrawCube({base.x, base.y + 1.10f, base.z}, 0.55f, 1.60f, 0.55f, stone);
  DrawCubeWires({base.x, base.y + 1.10f, base.z}, 0.55f, 1.60f, 0.55f,
               {160, 148, 120, 180});
  // Pyramid cap
  DrawCylinder({base.x, base.y + 1.90f, base.z}, 0.35f, 0.0f, 0.60f, 5, stone);
  // Glow ring
  float gr = 0.60f + pulse * 0.15f;
  Color gc = {(unsigned char)(glow.r),
              (unsigned char)(glow.g),
              (unsigned char)(glow.b),
              (unsigned char)(60 + (int)(pulse * 80))};
  DrawCylinder({base.x, base.y + 0.30f, base.z}, gr, gr, 0.04f, 16, gc);
}

static void DrawVegetationInstance(const VegetationInstance &vi) {
  Vector3 base = {vi.x, vi.cachedY, vi.z};
  switch (vi.type) {
    case VEG_SAGUARO:    DrawSaguaro(base, vi.scale);    break;
    case VEG_PRICKLY_PEAR: DrawPricklyPear(base, vi.scale); break;
    case VEG_AGAVE:      DrawAgave(base, vi.scale);      break;
    case VEG_DRY_TREE:   DrawDryTree(base, vi.scale);    break;
  }
}

// Draw tent shadow (large, soft PCF)
static void DrawTentShadow(const TentInstance &t) {
  float shadowX = t.worldX - g_sunDir.x * 4.0f * 0.5f;
  float shadowZ = t.worldZ - g_sunDir.z * 4.0f * 0.5f;
  // Simple single-layer tent shadow — skip per-sample terrain lookups
  float gy = GetDuneHeight(shadowX, shadowZ) + 0.005f;
  DrawCube({shadowX, gy, shadowZ}, 4.2f, 0.01f, 4.2f, {0, 0, 0, 12});
  DrawCube({shadowX, gy + 0.005f, shadowZ}, 4.0f, 0.01f, 4.0f, {0, 0, 0, 25});
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

  // 3. God rays — skip at night (sun below horizon, no visible rays)
  bool doGodrays = !g_worldClock.IsNight();
  if (doGodrays) {
    BeginTextureMode(g_godrayFBO);
    ClearBackground(BLACK);
    BeginShaderMode(g_shGodrays);
    float sunPos[2]; g_worldClock.GetCelestialUV(&sunPos[0], &sunPos[1]);
    SetShaderValue(g_shGodrays, g_locSunPos, sunPos, SHADER_UNIFORM_VEC2);
    DrawFBOQuadScaled(g_sceneFBO, SCREEN_W / 2, SCREEN_H / 2);
    EndShaderMode();
    EndTextureMode();
  }

  // 4. Final composite: scene + bloom + godrays + heat haze + ACES + vignette
  // Render into g_pixelFBO so we can pixelate the result
  BeginTextureMode(g_pixelFBO);
  ClearBackground(BLACK);
  BeginShaderMode(g_shComposite);
  // Bind bloom and godray textures to slots 1 and 2
  SetShaderValueTexture(g_shComposite, g_locBloomTex, g_blurB.texture);
  SetShaderValueTexture(g_shComposite, g_locGodrayTex, g_godrayFBO.texture);
  float bloomStr = 0.20f, godrayStr = doGodrays ? 0.08f : 0.0f;
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
  float n = PerlinFBM(x * DUNE_FREQUENCY, z * DUNE_FREQUENCY, 3, 0.5f);
  float h = n - 0.5f;
  float s = powf(fabsf(h), 1.6f);
  h = 0.7f * h + 0.3f * (h >= 0.0f ? s : -s);
  float base = h * DUNE_MAX_HEIGHT;

  // ── Ma'ayan: oasis depression (early-exit > 18 units) ────────────────────
  {
    float dx = x - CITY_MAAYAN_X, dz = z - CITY_MAAYAN_Z;
    float d2 = dx*dx + dz*dz;
    if (d2 < 324.0f) // 18*18
      base -= expf(-d2 / (10.0f*10.0f)) * 1.8f;
  }
  // ── Avak: flat eroded plain (early-exit > 20 units) ──────────────────────
  {
    float dx = x - CITY_AVAK_X, dz = z - CITY_AVAK_Z;
    float d2 = dx*dx + dz*dz;
    if (d2 < 400.0f) { // 20*20
      float flat = expf(-d2 / (13.0f*13.0f));
      base *= (1.0f - flat * 0.85f);
    }
  }
  // ── Gan: terraced hills (early-exit > 22 units) ───────────────────────────
  {
    float dx = x - CITY_GAN_X, dz = z - CITY_GAN_Z;
    float d2 = dx*dx + dz*dz;
    if (d2 < 484.0f) { // 22*22
      float dist      = sqrtf(d2);
      float influence = expf(-d2 / (14.0f*14.0f));
      float tier      = fmaxf(0.0f, floorf((14.0f - dist) / 3.0f));
      base += influence * tier * 0.5f;
    }
  }
  // ── Sela: canyon trench (early-exit > 20 units) ───────────────────────────
  {
    float dx = x - CITY_SELA_X, dz = z - CITY_SELA_Z;
    float d2 = dx*dx + dz*dz;
    if (d2 < 400.0f) { // 20*20
      float crossSect = expf(-(dz*dz) / (5.5f*5.5f))
                      * expf(-(dx*dx) / (13.0f*13.0f));
      base -= crossSect * 5.5f;
    }
  }

  return base;
}

// Bilinear sample matching the terrain mesh's 0.5-unit vertex grid.
// Prevents player from sinking into valleys where GetDuneHeight(exact pos)
// dips below the mesh's linearly-interpolated surface.
static float GetTerrainBilinear(float x, float z) {
  const float step = 0.5f;
  float kx = floorf((x + 0.5f) / step);
  float kz = floorf((z + 0.5f) / step);
  float x0 = kx * step - 0.5f, x1 = x0 + step;
  float z0 = kz * step - 0.5f, z1 = z0 + step;
  float fx = Clamp((x - x0) / step, 0.f, 1.f);
  float fz = Clamp((z - z0) / step, 0.f, 1.f);
  float h00 = GetDuneHeight(x0, z0), h10 = GetDuneHeight(x1, z0);
  float h01 = GetDuneHeight(x0, z1), h11 = GetDuneHeight(x1, z1);
  // Use the same diagonal split as GenerateTerrainMesh:
  //   T1 (upper-left): (x0,z0),(x1,z0),(x0,z1)  — when fx+fz <= 1
  //   T2 (lower-right): (x1,z0),(x1,z1),(x0,z1) — when fx+fz >  1
  // Bilinear would underestimate the mesh surface when h00+h11 < h10+h01,
  // causing the player to clip through the terrain.
  if (fx + fz <= 1.0f)
    return h00 * (1.f - fx - fz) + h10 * fx + h01 * fz;
  else
    return h10 * (1.f - fz) + h11 * (fx + fz - 1.f) + h01 * (1.f - fx);
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
  int resX = MAP_W * 2, resZ = MAP_H * 2; // 2x subdivision (larger map — still smooth)
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

  // Smooth vertex normals: central-difference gradient per grid vertex
  std::vector<Vector3> smoothNormals((resX + 1) * (resZ + 1));
  for (int zy = 0; zy <= resZ; zy++) {
    for (int zx = 0; zx <= resX; zx++) {
      float hL = heights[zy*(resX+1) + (zx > 0    ? zx-1 : zx)];
      float hR = heights[zy*(resX+1) + (zx < resX ? zx+1 : zx)];
      float hD = heights[(zy > 0    ? zy-1 : zy)*(resX+1) + zx];
      float hU = heights[(zy < resZ ? zy+1 : zy)*(resX+1) + zx];
      Vector3 dX = {stepX * 2.0f, hR - hL, 0.0f};
      Vector3 dZ = {0.0f,         hU - hD, stepZ * 2.0f};
      Vector3 sn = {dX.y*dZ.z - dX.z*dZ.y,
                    dX.z*dZ.x - dX.x*dZ.z,
                    dX.x*dZ.y - dX.y*dZ.x};
      float snl = sqrtf(sn.x*sn.x + sn.y*sn.y + sn.z*sn.z);
      if (snl > 0.001f) { sn.x /= snl; sn.y /= snl; sn.z /= snl; }
      smoothNormals[zy*(resX+1)+zx] = sn;
    }
  }

  // Helper: get precomputed smooth normal at grid vertex (gx, gz)
  auto getSN = [&](int gx, int gz) -> Vector3 {
    return smoothNormals[gz*(resX+1)+gx];
  };

  int vi = 0;
  for (int zy = 0; zy < resZ; zy++)
    for (int zx = 0; zx < resX; zx++) {
      float x0 = zx * stepX - 0.5f, x1 = (zx + 1) * stepX - 0.5f;
      float z0 = zy * stepZ - 0.5f, z1 = (zy + 1) * stepZ - 0.5f;
      float h00 = heights[zy * (resX + 1) + zx];
      float h10 = heights[zy * (resX + 1) + zx + 1];
      float h01 = heights[(zy + 1) * (resX + 1) + zx];
      float h11 = heights[(zy + 1) * (resX + 1) + zx + 1];
      float u0 = x0, u1 = x1, v0 = z0, v1 = z1;

      // Triangle 1: vertices (zx,zy), (zx+1,zy), (zx,zy+1)
      Vector3 pts1[3] = {{x0,h00,z0},{x1,h10,z0},{x0,h01,z1}};
      Vector3 nrm1[3] = {getSN(zx,zy), getSN(zx+1,zy), getSN(zx,zy+1)};
      float uvs1[6] = {u0, v0, u1, v0, u0, v1};
      for (int k = 0; k < 3; k++) {
        mesh.vertices[vi * 3]     = pts1[k].x;
        mesh.vertices[vi * 3 + 1] = pts1[k].y;
        mesh.vertices[vi * 3 + 2] = pts1[k].z;
        mesh.normals[vi * 3]      = nrm1[k].x;
        mesh.normals[vi * 3 + 1]  = nrm1[k].y;
        mesh.normals[vi * 3 + 2]  = nrm1[k].z;
        mesh.texcoords[vi * 2]     = uvs1[k * 2] * 0.25f;
        mesh.texcoords[vi * 2 + 1] = uvs1[k * 2 + 1] * 0.25f;
        float sunDot = Clamp(nrm1[k].x * (-g_sunDir.x) + nrm1[k].y * (-g_sunDir.y) +
                             nrm1[k].z * (-g_sunDir.z), 0, 1);
        float light = 0.55f + 0.45f * sunDot;
        float yNorm = (pts1[k].y - minH) / (maxH - minH + 1e-5f);
        light *= 0.82f + 0.30f * yNorm;
        mesh.colors[vi * 4 + 0] = (unsigned char)Clamp(210 * light, 0, 255);
        mesh.colors[vi * 4 + 1] = (unsigned char)Clamp(185 * light, 0, 255);
        mesh.colors[vi * 4 + 2] = (unsigned char)Clamp(140 * light, 0, 255);
        mesh.colors[vi * 4 + 3] = 255;
        vi++;
      }

      // Triangle 2: vertices (zx+1,zy), (zx+1,zy+1), (zx,zy+1)
      Vector3 pts2[3] = {{x1,h10,z0},{x1,h11,z1},{x0,h01,z1}};
      Vector3 nrm2[3] = {getSN(zx+1,zy), getSN(zx+1,zy+1), getSN(zx,zy+1)};
      float uvs2[6] = {u1, v0, u1, v1, u0, v1};
      for (int k = 0; k < 3; k++) {
        mesh.vertices[vi * 3]     = pts2[k].x;
        mesh.vertices[vi * 3 + 1] = pts2[k].y;
        mesh.vertices[vi * 3 + 2] = pts2[k].z;
        mesh.normals[vi * 3]      = nrm2[k].x;
        mesh.normals[vi * 3 + 1]  = nrm2[k].y;
        mesh.normals[vi * 3 + 2]  = nrm2[k].z;
        mesh.texcoords[vi * 2]     = uvs2[k * 2] * 0.25f;
        mesh.texcoords[vi * 2 + 1] = uvs2[k * 2 + 1] * 0.25f;
        float sunDot = Clamp(nrm2[k].x * (-g_sunDir.x) + nrm2[k].y * (-g_sunDir.y) +
                             nrm2[k].z * (-g_sunDir.z), 0, 1);
        float light = 0.55f + 0.45f * sunDot;
        float yNorm = (pts2[k].y - minH) / (maxH - minH + 1e-5f);
        light *= 0.82f + 0.30f * yNorm;
        mesh.colors[vi * 4 + 0] = (unsigned char)Clamp(210 * light, 0, 255);
        mesh.colors[vi * 4 + 1] = (unsigned char)Clamp(185 * light, 0, 255);
        mesh.colors[vi * 4 + 2] = (unsigned char)Clamp(140 * light, 0, 255);
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
  Vector3 sdir = g_sunDir;
  SetShaderValue(g_shTriplanar, g_locTripSunDir, &sdir, SHADER_UNIFORM_VEC3);
  g_terrainReady = true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// §7  OVERWORLD — Init / Update / Draw
// ═══════════════════════════════════════════════════════════════════════════════


// ─── InitNorthernWastes — organic cactus/tree clusters in z < 14 ─────────────
static void InitNorthernWastes() {
  g_numVeg = 0;
  // Cluster centres: (cx, cz, radius, count, preferred type)
  struct Cluster { float cx, cz, rad; int count; VegType pref; };
  static constexpr Cluster clusters[] = {
    // ── Northern Wastes (z < 65) ────────────────────────────────────
    { 50.0f, 12.5f, 14.0f, 7, VEG_SAGUARO},      // NW saguaro grove
    {150.0f, 10.0f, 15.0f, 6, VEG_DRY_TREE},      // NE dead-tree cluster
    {100.0f, 27.5f, 12.5f, 8, VEG_AGAVE},         // central agave field
    { 30.0f, 45.0f, 11.0f, 6, VEG_PRICKLY_PEAR},  // western prickly pear
    {170.0f, 47.5f, 11.0f, 6, VEG_PRICKLY_PEAR},  // eastern prickly pear
    { 75.0f, 60.0f, 12.5f, 5, VEG_SAGUARO},       // approach sentinels L
    {125.0f, 60.0f, 12.5f, 5, VEG_DRY_TREE},      // approach sentinels R
    // ── Southern Desert (z > 95) — sandstorm-scoured ────────────────
    {100.0f,105.0f, 14.0f, 7, VEG_DRY_TREE},      // south centre dead grove
    { 45.0f,115.0f, 12.0f, 6, VEG_AGAVE},         // SW agave
    {155.0f,115.0f, 12.0f, 6, VEG_PRICKLY_PEAR},  // SE prickly pear
    { 75.0f,130.0f, 10.0f, 5, VEG_DRY_TREE},      // deep south trees L
    {130.0f,130.0f, 10.0f, 5, VEG_DRY_TREE},      // deep south trees R
  };
  static constexpr int NUM_CLUSTERS =
      (int)(sizeof(clusters) / sizeof(clusters[0]));
  for (int ci = 0; ci < NUM_CLUSTERS && g_numVeg < MAX_VEGETATION; ci++) {
    const Cluster &cl = clusters[ci];
    for (int k = 0; k < cl.count && g_numVeg < MAX_VEGETATION; k++) {
      float angle  = (float)GetRandomValue(0, 628) * 0.01f;
      float dist   = (float)GetRandomValue(0, (int)(cl.rad * 100)) * 0.01f;
      float px = cl.cx + cosf(angle) * dist;
      float pz = cl.cz + sinf(angle) * dist;
      // Clamp to map + clear of tents
      px = Clamp(px, 1.5f, (float)(MAP_W - 2));
      // Clamp to valid terrain + keep clusters in their respective zones
      pz = Clamp(pz, 1.0f, (float)(MAP_H - 2));
      // Skip if too close to a tent center
      bool nearTent = false;
      for (int ti = 0; ti < g_numTents; ti++) {
        float dx = px - g_tentInstances[ti].worldX;
        float dz = pz - g_tentInstances[ti].worldZ;
        if (dx*dx + dz*dz < 9.0f) { nearTent = true; break; }
      }
      if (nearTent) continue;
      VegetationInstance &vi = g_vegInstances[g_numVeg++];
      vi.x = px;
      vi.z = pz;
      vi.rotY = (float)GetRandomValue(0, 360);
      // Mix: 70% preferred type, 30% random
      if (GetRandomValue(0, 9) < 7)
        vi.type = cl.pref;
      else
        vi.type = (VegType)GetRandomValue(0, 3);
      // Scale varies by type
      float baseScale = (vi.type == VEG_SAGUARO || vi.type == VEG_DRY_TREE)
                        ? 0.65f : 0.50f;
      vi.scale = baseScale + (float)GetRandomValue(0, 30) * 0.01f;
      vi.colliderRadius = vi.scale * 0.45f;
      vi.cachedY = GetDuneHeight(vi.x, vi.z);
    }
  }
}

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

  // ── Village cluster — tents spaced ≥10 tiles apart, clear of well/NPCs ─────
  //   Spread across a 36×18 patch centred near (96, 58).
  //   House of Glory (hs=3) occupies roughly (97-103, 66-72) — well south of shops.
  PlaceTent    ( 80, 48, {190, 170, 130, 255}, {160, 80, 40, 255},  "Elder's Tent",    SCENE_TENT1);
  PlaceTent    ( 93, 46, {175, 155, 110, 255}, {200, 160, 30, 255}, "Card Shop",       SCENE_TENT2);
  PlaceTent    (108, 48, {180, 160, 120, 255}, {80, 160, 100, 255}, "Healer's Tent",   SCENE_TENT3);
  PlaceTent    ( 80, 62, {165, 145, 105, 255}, {130, 80, 160, 255}, "Rest House",      SCENE_TENT4);
  PlaceTentSized(100, 66, 3.0f, {200, 175, 130, 255}, {140, 40, 40, 255}, "House of Glory", SCENE_TENT1);

  // ── Distant explorer tents at map corners ────────────────────────────────
  PlaceTent(22,  14, {180, 140, 90, 255},  {160, 40,  40, 255}, "Fatima's Tent",   SCENE_TENT1);
  PlaceTent(138, 14, {160, 150, 120, 255}, {50,  80, 170, 255}, "Scholar's Study", SCENE_TENT2);
  PlaceTent(22,  99, {170, 120, 80, 255},  {190, 160, 40, 255}, "Yara's Bazaar",   SCENE_TENT3);
  PlaceTent(138, 99, {150, 135, 100, 255}, {80, 140,  80, 255}, "Kai's Workshop",  SCENE_TENT4);

  // ── Scattered tents (exploration) ────────────────────────────────────────
  PlaceTent(46,  44, {160, 135, 95, 255},  {180, 60, 60, 255},  "Dune Shelter",  SCENE_TENT1);
  PlaceTent(136, 46, {155, 140, 105, 255}, {60, 140, 60, 255},  "East Outpost",  SCENE_TENT2);
  PlaceTent(74, 110, {165, 130, 85, 255},  {160, 120, 40, 255}, "South Camp",    SCENE_TENT3);

  // ── NPC placement ────────────────────────────────────────────────────────
  g_numNpcs = 0;
  for (int i = 0; i < 60; i++) {
    g_npcs[i].nomadic = false;
    g_npcs[i].nomadicItem = -1;
    g_npcs[i].waitTimer = 0.0f;
  }

  // --- Village NPCs ---
  // Elder Aziz (outside Elder's Tent at 80,48 — door south at z=52)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=82; n.gy=53; n.worldX=82.f; n.worldZ=53.f; n.name="Elder Aziz";
    n.shirtCol={160,130,70,255}; n.pantsCol={80,70,50,255}; n.hatCol={160,130,70,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({190,155,110,255},{50,40,25,255},{160,130,70,255},
                              {200,180,100,255},{80,70,50,255},{100,80,45,255}); }

  // Merchant (outside Card Shop at 93,46 — door south at z=50)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=95; n.gy=51; n.worldX=95.f; n.worldZ=51.f; n.name="Merchant";
    n.shirtCol={200,80,40,255}; n.pantsCol={100,80,50,255}; n.hatCol={200,80,40,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({175,140,95,255},{35,28,18,255},{200,80,40,255},
                              {240,180,50,255},{100,80,50,255},{80,60,35,255}); }

  // Healer (outside Healer's Tent at 108,48 — door south at z=52)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=110; n.gy=53; n.worldX=110.f; n.worldZ=53.f; n.name="Healer";
    n.shirtCol={60,160,120,255}; n.pantsCol={180,175,165,255}; n.hatCol={60,160,120,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({160,125,85,255},{40,30,20,255},{60,160,120,255},
                              {200,230,210,255},{180,175,165,255},{90,70,50,255}); }

  // Guard (south entrance — just below HOG at 100,66,hs=3; door at z=72)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=103; n.gy=74; n.worldX=103.f; n.worldZ=74.f; n.name="Guard";
    n.shirtCol={60,70,140,255}; n.pantsCol={50,50,60,255}; n.hatCol={80,90,160,255};
    n.dir=DIR_UP; n.facingAngle=n.targetAngle=DirToAngle(DIR_UP);
    n.colors=MakeChibiColors({175,140,100,255},{30,25,18,255},{60,70,140,255},
                              {200,200,220,255},{50,50,60,255},{60,55,45,255}); }

  // Water Keeper (near well at 98,62 — clear of all tents)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=96; n.gy=63; n.worldX=96.f; n.worldZ=63.f; n.name="Water Keeper";
    n.shirtCol={80,130,170,255}; n.pantsCol={110,100,80,255}; n.hatCol={70,110,150,255};
    n.dir=DIR_RIGHT; n.facingAngle=n.targetAngle=DirToAngle(DIR_RIGHT);
    n.colors=MakeChibiColors({170,135,95,255},{40,32,20,255},{80,130,170,255},
                              {120,180,220,255},{110,100,80,255},{85,70,50,255}); }

  // Wanderer (south of Rest House at 80,62 — door at z=66)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=83; n.gy=68; n.worldX=83.f; n.worldZ=68.f; n.name="Wanderer";
    n.shirtCol={140,100,60,255}; n.pantsCol={90,80,60,255}; n.hatCol={120,90,50,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({180,145,100,255},{55,42,28,255},{140,100,60,255},
                              {180,150,90,255},{90,80,60,255},{100,80,50,255}); }

  // --- Village tent interior NPCs ---
  // Elder's Tent interior (gx=80,gy=48 → center 82,50)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=82; n.gy=49; n.worldX=82.0f; n.worldZ=49.5f; n.name="Village Elder";
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({195,160,115,255},{70,55,35,255},{180,150,80,255},
                              {220,200,140,255},{100,85,60,255},{110,90,55,255}); }

  // Card Shop interior (gx=93,gy=46 → center 95,48): pack seller + assistant
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=95; n.gy=47; n.worldX=95.0f; n.worldZ=47.5f; n.name="Pack Seller";
    n.role=NPC_ROLE_SHOP; n.cityIndex=-1; // village shop, not a city
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({175,140,95,255},{30,22,12,255},{50,100,170,255},
                              {50,100,170,255},{70,60,45,255},{80,65,40,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=96; n.gy=47; n.worldX=96.5f; n.worldZ=47.8f; n.name="Shop Apprentice";
    n.dir=DIR_LEFT; n.facingAngle=n.targetAngle=DirToAngle(DIR_LEFT);
    n.colors=MakeChibiColors({185,148,105,255},{45,32,18,255},{80,140,60,255},
                              {80,140,60,255},{90,75,55,255},{85,70,45,255}); }

  // Healer's Tent interior (gx=108,gy=48 → center 110,50)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=110; n.gy=49; n.worldX=110.0f; n.worldZ=49.5f; n.name="Herbalist";
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({165,130,90,255},{40,30,18,255},{55,150,110,255},
                              {200,230,205,255},{170,168,155,255},{90,72,48,255}); }

  // Rest House interior (gx=80,gy=62 → center 82,64)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=82; n.gy=63; n.worldX=82.0f; n.worldZ=63.5f; n.name="Innkeeper";
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({175,142,98,255},{50,38,22,255},{130,80,160,255},
                              {190,165,210,255},{110,100,80,255},{95,78,50,255}); }

  // House of Glory interior (gx=100,gy=66,hs=3 → center 103,69)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=103; n.gy=67; n.worldX=103.0f; n.worldZ=67.5f; n.name="Registrar";
    n.role=NPC_ROLE_TOURNAMENT;
    n.cityIndex=0;
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({190,155,110,255},{35,28,15,255},{180,30,30,255},
                              {220,200,160,255},{80,70,52,255},{100,82,50,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=103; n.gy=69; n.worldX=103.0f; n.worldZ=69.5f; n.name="Duel Judge";
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({180,145,100,255},{40,30,15,255},{60,60,130,255},
                              {200,200,230,255},{65,60,50,255},{90,75,45,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=101; n.gy=68; n.worldX=101.5f; n.worldZ=68.0f; n.name="Card Trader";
    n.dir=DIR_RIGHT; n.facingAngle=n.targetAngle=DirToAngle(DIR_RIGHT);
    n.colors=MakeChibiColors({170,135,92,255},{55,42,25,255},{150,120,40,255},
                              {210,175,50,255},{95,82,58,255},{88,72,45,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=105; n.gy=68; n.worldX=105.0f; n.worldZ=68.0f; n.name="Spectator";
    n.dir=DIR_LEFT; n.facingAngle=n.targetAngle=DirToAngle(DIR_LEFT);
    n.colors=MakeChibiColors({185,150,105,255},{45,34,20,255},{100,160,80,255},
                              {130,190,110,255},{90,80,58,255},{92,76,48,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=101; n.gy=70; n.worldX=101.5f; n.worldZ=70.0f; n.name="Duelist";
    n.dir=DIR_UP; n.facingAngle=n.targetAngle=DirToAngle(DIR_UP);
    n.colors=MakeChibiColors({180,148,102,255},{38,28,16,255},{200,80,40,255},
                              {230,120,60,255},{88,78,55,255},{95,78,48,255}); }

  // --- Distant corner NPCs (original 4) ---
  // Fatima (outside Fatima's Tent at 22,14 — door at z=18)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=24; n.gy=19; n.worldX=24.f; n.worldZ=19.f; n.name="Fatima";
    n.shirtCol={180,140,60,255}; n.pantsCol={90,70,50,255}; n.hatCol={180,140,60,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({180,140,100,255},{60,40,25,255},{180,140,60,255},
                              {140,100,50,255},{90,70,50,255},{160,120,60,255}); }

  // Scholar (outside Scholar's Study at 138,14 — door at z=18)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=140; n.gy=19; n.worldX=140.f; n.worldZ=19.f; n.name="Scholar";
    n.shirtCol={60,160,160,255}; n.pantsCol={180,180,170,255}; n.hatCol={60,160,160,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({140,100,70,255},{30,25,20,255},{60,160,160,255},
                              {200,220,220,255},{180,180,170,255},{100,80,60,255}); }

  // Yara (outside Yara's Bazaar at 22,99 — door at z=103)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=24; n.gy=104; n.worldX=24.f; n.worldZ=104.f; n.name="Yara";
    n.shirtCol={80,30,30,255}; n.pantsCol={50,45,40,255}; n.hatCol={100,30,30,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({160,120,80,255},{40,30,20,255},{100,30,30,255},
                              {80,30,30,255},{50,45,40,255},{60,50,40,255}); }

  // Kai (outside Kai's Workshop at 138,99 — door at z=103)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=140; n.gy=104; n.worldX=140.f; n.worldZ=104.f; n.name="Kai";
    n.shirtCol={80,140,60,255}; n.pantsCol={120,110,80,255}; n.hatCol={80,140,60,255};
    n.dir=DIR_LEFT; n.facingAngle=n.targetAngle=DirToAngle(DIR_LEFT);
    n.colors=MakeChibiColors({200,160,120,255},{50,35,20,255},{80,140,60,255},
                              {140,160,100,255},{120,110,80,255},{140,100,50,255}); }

  // --- Scattered NPCs across the map ---
  // Dune Walker (mid-west)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=55; n.gy=50; n.worldX=55.f; n.worldZ=50.f; n.name="Dune Walker";
    n.shirtCol={150,120,70,255}; n.pantsCol={100,90,65,255}; n.hatCol={170,140,80,255};
    n.dir=DIR_RIGHT; n.facingAngle=n.targetAngle=DirToAngle(DIR_RIGHT);
    n.colors=MakeChibiColors({185,150,105,255},{45,35,22,255},{150,120,70,255},
                              {200,170,90,255},{100,90,65,255},{110,85,55,255}); }

  // Desert Sage (mid-east)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=145; n.gy=50; n.worldX=145.f; n.worldZ=50.f; n.name="Desert Sage";
    n.shirtCol={120,80,160,255}; n.pantsCol={160,150,130,255}; n.hatCol={100,60,140,255};
    n.dir=DIR_LEFT; n.facingAngle=n.targetAngle=DirToAngle(DIR_LEFT);
    n.colors=MakeChibiColors({165,130,90,255},{35,28,18,255},{120,80,160,255},
                              {210,190,240,255},{160,150,130,255},{90,75,55,255}); }

  // Sand Poet (south-west)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=52; n.gy=112; n.worldX=52.f; n.worldZ=112.f; n.name="Sand Poet";
    n.shirtCol={180,160,60,255}; n.pantsCol={80,75,55,255}; n.hatCol={160,140,45,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({170,135,95,255},{40,32,20,255},{180,160,60,255},
                              {230,210,80,255},{80,75,55,255},{95,75,50,255}); }

  // Horizon Scout (south-east)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=148; n.gy=90; n.worldX=148.f; n.worldZ=90.f; n.name="Horizon Scout";
    n.shirtCol={40,120,80,255}; n.pantsCol={70,60,40,255}; n.hatCol={30,100,60,255};
    n.dir=DIR_LEFT; n.facingAngle=n.targetAngle=DirToAngle(DIR_LEFT);
    n.colors=MakeChibiColors({175,140,100,255},{35,28,18,255},{40,120,80,255},
                              {80,200,130,255},{70,60,40,255},{80,65,45,255}); }

  // ── Nomadic Travellers (wander the roads between cities) ──────────────────
  // Nomad 1: Ruby — sells Blessed Amulet
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=58; n.gy=50; n.worldX=58.f; n.worldZ=50.f; n.name="Ruby the Wanderer";
    n.shirtCol={180,40,40,255}; n.pantsCol={100,70,50,255}; n.hatCol={160,30,30,255};
    n.dir=DIR_RIGHT; n.facingAngle=n.targetAngle=DirToAngle(DIR_RIGHT);
    n.colors=MakeChibiColors({178,142,100,255},{48,36,22,255},{180,40,40,255},
                              {220,60,60,255},{100,70,50,255},{100,75,50,255});
    n.nomadic=true; n.destX=CITY_MAAYAN_X+3; n.destZ=CITY_MAAYAN_Z+3;
    n.waitTimer=0.0f; n.nomadicItem=ITEM_BLESSED_AMULET; }

  // Nomad 2: Cassius — sells Swift Boots
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=130; n.gy=60; n.worldX=130.f; n.worldZ=60.f; n.name="Cassius the Merchant";
    n.shirtCol={60,80,160,255}; n.pantsCol={90,80,60,255}; n.hatCol={50,65,140,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({182,146,103,255},{46,35,22,255},{60,80,160,255},
                              {140,155,220,255},{90,80,60,255},{105,80,52,255});
    n.nomadic=true; n.destX=CITY_GAN_X-3; n.destZ=CITY_GAN_Z+3;
    n.waitTimer=15.0f; n.nomadicItem=ITEM_SWIFT_BOOTS; }

  // Nomad 3: Miriam — sells Fair Scale
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=70; n.gy=100; n.worldX=70.f; n.worldZ=100.f; n.name="Miriam the Nomad";
    n.shirtCol={180,160,40,255}; n.pantsCol={100,90,65,255}; n.hatCol={160,140,30,255};
    n.dir=DIR_UP; n.facingAngle=n.targetAngle=DirToAngle(DIR_UP);
    n.colors=MakeChibiColors({175,140,98,255},{42,32,20,255},{180,160,40,255},
                              {230,215,80,255},{100,90,65,255},{98,76,50,255});
    n.nomadic=true; n.destX=CITY_AVAK_X+4; n.destZ=CITY_AVAK_Z-4;
    n.waitTimer=8.0f; n.nomadicItem=ITEM_FAIR_SCALE; }

  // Nomad 4: Doran — sells Seller's Certificate
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=110; n.gy=40; n.worldX=110.f; n.worldZ=40.f; n.name="Doran the Peddler";
    n.shirtCol={100,160,80,255}; n.pantsCol={80,70,50,255}; n.hatCol={80,140,60,255};
    n.dir=DIR_LEFT; n.facingAngle=n.targetAngle=DirToAngle(DIR_LEFT);
    n.colors=MakeChibiColors({180,145,102,255},{44,34,21,255},{100,160,80,255},
                              {160,210,120,255},{80,70,50,255},{102,78,52,255});
    n.nomadic=true; n.destX=CITY_SELA_X+2; n.destZ=CITY_SELA_Z+2;
    n.waitTimer=22.0f; n.nomadicItem=ITEM_SELLERS_CERT; }

  // ── City 2: Ma'ayan — tents + NPCs ───────────────────────────────────────
  PlaceTent(16, 69, {195, 188, 170, 255}, {60, 130, 180, 255}, "Spring Lodge",    SCENE_TENT3);
  PlaceTent(28, 69, {188, 182, 162, 255}, {180, 155, 55, 255}, "Oasis Inn",       SCENE_TENT4);
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=22;  n.gy=72; n.worldX=22.f;  n.worldZ=72.f;  n.name="Spring Guardian";
    n.shirtCol={60,160,185,255}; n.pantsCol={80,120,150,255}; n.hatCol={40,140,165,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({165,130,90,255},{38,28,18,255},{60,160,185,255},
                              {140,210,230,255},{80,120,150,255},{85,68,48,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=18;  n.gy=78; n.worldX=18.f;  n.worldZ=78.f;  n.name="Oasis Trader";
    n.shirtCol={220,140,40,255}; n.pantsCol={160,120,70,255}; n.hatCol={200,100,30,255};
    n.dir=DIR_RIGHT; n.facingAngle=n.targetAngle=DirToAngle(DIR_RIGHT);
    n.colors=MakeChibiColors({178,142,100,255},{42,32,20,255},{220,140,40,255},
                              {240,190,90,255},{160,120,70,255},{95,72,48,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=26;  n.gy=78; n.worldX=26.f;  n.worldZ=78.f;  n.name="Well Keeper";
    n.shirtCol={130,160,140,255}; n.pantsCol={100,130,110,255}; n.hatCol={90,120,100,255};
    n.dir=DIR_LEFT; n.facingAngle=n.targetAngle=DirToAngle(DIR_LEFT);
    n.colors=MakeChibiColors({172,136,95,255},{38,28,18,255},{130,160,140,255},
                              {200,220,205,255},{100,130,110,255},{88,68,48,255}); }

  // ── City 3: Avak — tents + NPCs ──────────────────────────────────────────
  PlaceTent(94, 127, {185, 175, 150, 255}, {140, 90, 50, 255}, "Ruin Shelter",    SCENE_TENT1);
  PlaceTent(106,127, {178, 168, 142, 255}, {110, 70, 40, 255}, "Drifter's Rest",  SCENE_TENT2);
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=100; n.gy=131; n.worldX=100.f; n.worldZ=131.f; n.name="Ruin Keeper";
    n.shirtCol={160,155,138,255}; n.pantsCol={120,112,94,255}; n.hatCol={145,138,120,255};
    n.dir=DIR_UP; n.facingAngle=n.targetAngle=DirToAngle(DIR_UP);
    n.colors=MakeChibiColors({168,132,92,255},{35,27,18,255},{160,155,138,255},
                              {200,195,178,255},{120,112,94,255},{88,68,46,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=96;  n.gy=135; n.worldX=96.f;  n.worldZ=135.f; n.name="Dust Drifter";
    n.shirtCol={148,130,98,255};  n.pantsCol={110,96,72,255};  n.hatCol={168,148,110,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({175,140,98,255},{40,30,20,255},{148,130,98,255},
                              {195,178,138,255},{110,96,72,255},{90,70,48,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=104; n.gy=135; n.worldX=104.f; n.worldZ=135.f; n.name="Ancient Scribe";
    n.shirtCol={200,192,170,255}; n.pantsCol={175,168,148,255}; n.hatCol={160,152,132,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({168,132,90,255},{38,28,18,255},{200,192,170,255},
                              {230,225,205,255},{175,168,148,255},{90,70,48,255}); }

  // ── City 4: Gan — tents + NPCs ───────────────────────────────────────────
  PlaceTent(171, 69, {175, 158, 118, 255}, {80, 155, 65, 255},  "Gardener's Rest", SCENE_TENT3);
  PlaceTent(183, 69, {170, 152, 112, 255}, {100, 175, 80, 255}, "Harvest Inn",     SCENE_TENT4);
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=177; n.gy=72; n.worldX=177.f; n.worldZ=72.f; n.name="Master Gardener";
    n.shirtCol={80,148,55,255}; n.pantsCol={100,130,60,255}; n.hatCol={60,125,40,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({172,138,96,255},{40,30,20,255},{80,148,55,255},
                              {140,200,100,255},{100,130,60,255},{85,65,45,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=173; n.gy=78; n.worldX=173.f; n.worldZ=78.f; n.name="Irrigation Eng.";
    n.shirtCol={120,140,160,255}; n.pantsCol={90,110,120,255}; n.hatCol={100,120,140,255};
    n.dir=DIR_RIGHT; n.facingAngle=n.targetAngle=DirToAngle(DIR_RIGHT);
    n.colors=MakeChibiColors({170,135,95,255},{38,28,18,255},{120,140,160,255},
                              {180,195,210,255},{90,110,120,255},{85,65,45,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=181; n.gy=78; n.worldX=181.f; n.worldZ=78.f; n.name="Orchard Keeper";
    n.shirtCol={190,130,55,255}; n.pantsCol={140,100,48,255}; n.hatCol={170,115,42,255};
    n.dir=DIR_LEFT; n.facingAngle=n.targetAngle=DirToAngle(DIR_LEFT);
    n.colors=MakeChibiColors({175,140,98,255},{42,32,20,255},{190,130,55,255},
                              {230,180,90,255},{140,100,48,255},{88,68,48,255}); }

  // ── City 5: Sela — tents + NPCs ──────────────────────────────────────────
  PlaceTent(29, 24, {155, 138, 108, 255}, {165, 110, 35, 255}, "Miner's Lodge",   SCENE_TENT1);
  PlaceTent(41, 24, {148, 132, 102, 255}, {140,  95, 28, 255}, "Chain House",     SCENE_TENT2);
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=35;  n.gy=28; n.worldX=35.f;  n.worldZ=28.f; n.name="Chief Miner";
    n.shirtCol={80,68,52,255}; n.pantsCol={62,52,40,255}; n.hatCol={165,110,35,255};
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.colors=MakeChibiColors({165,130,88,255},{35,28,18,255},{80,68,52,255},
                              {165,110,35,255},{62,52,40,255},{82,62,42,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=31;  n.gy=32; n.worldX=31.f;  n.worldZ=32.f; n.name="Chain Keeper";
    n.shirtCol={100,80,55,255}; n.pantsCol={75,60,42,255}; n.hatCol={155,102,30,255};
    n.dir=DIR_RIGHT; n.facingAngle=n.targetAngle=DirToAngle(DIR_RIGHT);
    n.colors=MakeChibiColors({168,132,92,255},{38,28,18,255},{100,80,55,255},
                              {175,118,38,255},{75,60,42,255},{85,65,45,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=39;  n.gy=32; n.worldX=39.f;  n.worldZ=32.f; n.name="Brass Smith";
    n.shirtCol={60,52,42,255}; n.pantsCol={50,42,32,255}; n.hatCol={175,112,38,255};
    n.dir=DIR_LEFT; n.facingAngle=n.targetAngle=DirToAngle(DIR_LEFT);
    n.colors=MakeChibiColors({170,135,94,255},{36,28,18,255},{60,52,42,255},
                              {175,112,38,255},{50,42,32,255},{80,60,40,255}); }

  // ── City NPCs: Bazaar Merchant + Tournament Master per city ─────────────────
  // City 1: Ma'ayan (22, 75)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=28; n.gy=79; n.worldX=28.f; n.worldZ=79.f; n.name="Ma'ayan Merchant";
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.role=NPC_ROLE_SHOP; n.cityIndex=1;
    n.shirtCol={40,150,120,255}; n.pantsCol={80,100,80,255}; n.hatCol={40,150,120,255};
    n.colors=MakeChibiColors({170,135,95,255},{35,28,18,255},{40,150,120,255},
                              {80,210,180,255},{80,100,80,255},{75,60,45,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=16; n.gy=79; n.worldX=16.f; n.worldZ=79.f; n.name="Ma'ayan Tournament Master";
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.role=NPC_ROLE_TOURNAMENT; n.cityIndex=1;
    n.shirtCol={160,60,160,255}; n.pantsCol={60,40,60,255}; n.hatCol={200,80,200,255};
    n.colors=MakeChibiColors({160,125,85,255},{30,25,18,255},{160,60,160,255},
                              {220,160,220,255},{60,40,60,255},{70,55,40,255}); }
  // City 2: Avak (100, 133)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=106; n.gy=137; n.worldX=106.f; n.worldZ=137.f; n.name="Avak Merchant";
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.role=NPC_ROLE_SHOP; n.cityIndex=2;
    n.shirtCol={180,80,40,255}; n.pantsCol={90,70,50,255}; n.hatCol={200,100,50,255};
    n.colors=MakeChibiColors({180,145,100,255},{40,32,20,255},{180,80,40,255},
                              {230,130,70,255},{90,70,50,255},{95,75,50,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=94; n.gy=137; n.worldX=94.f; n.worldZ=137.f; n.name="Avak Tournament Master";
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.role=NPC_ROLE_TOURNAMENT; n.cityIndex=2;
    n.shirtCol={120,120,40,255}; n.pantsCol={80,80,50,255}; n.hatCol={160,160,50,255};
    n.colors=MakeChibiColors({175,140,100,255},{35,28,18,255},{120,120,40,255},
                              {210,210,80,255},{80,80,50,255},{80,65,45,255}); }
  // City 3: Gan (177, 75)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=183; n.gy=79; n.worldX=183.f; n.worldZ=79.f; n.name="Gan Merchant";
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.role=NPC_ROLE_SHOP; n.cityIndex=3;
    n.shirtCol={60,160,60,255}; n.pantsCol={80,110,80,255}; n.hatCol={60,180,60,255};
    n.colors=MakeChibiColors({165,130,90,255},{35,28,18,255},{60,160,60,255},
                              {100,220,100,255},{80,110,80,255},{70,55,40,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=171; n.gy=79; n.worldX=171.f; n.worldZ=79.f; n.name="Gan Tournament Master";
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.role=NPC_ROLE_TOURNAMENT; n.cityIndex=3;
    n.shirtCol={40,100,160,255}; n.pantsCol={40,60,80,255}; n.hatCol={50,130,200,255};
    n.colors=MakeChibiColors({170,135,95,255},{35,28,18,255},{40,100,160,255},
                              {80,180,240,255},{40,60,80,255},{70,55,40,255}); }
  // City 4: Sela (35, 30)
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=41; n.gy=33; n.worldX=41.f; n.worldZ=33.f; n.name="Sela Merchant";
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.role=NPC_ROLE_SHOP; n.cityIndex=4;
    n.shirtCol={160,100,60,255}; n.pantsCol={100,80,60,255}; n.hatCol={200,130,70,255};
    n.colors=MakeChibiColors({180,145,100,255},{40,32,20,255},{160,100,60,255},
                              {220,160,90,255},{100,80,60,255},{90,70,50,255}); }
  { NPC &n = g_npcs[g_numNpcs++];
    n.gx=29; n.gy=33; n.worldX=29.f; n.worldZ=33.f; n.name="Sela Tournament Master";
    n.dir=DIR_DOWN; n.facingAngle=n.targetAngle=DirToAngle(DIR_DOWN);
    n.role=NPC_ROLE_TOURNAMENT; n.cityIndex=4;
    n.shirtCol={160,40,40,255}; n.pantsCol={80,30,30,255}; n.hatCol={200,60,60,255};
    n.colors=MakeChibiColors({175,140,100,255},{35,28,18,255},{160,40,40,255},
                              {240,100,100,255},{80,30,30,255},{75,60,45,255}); }

  // Procedural rocks
  InitProceduralRocks();

  // Generate dune terrain mesh
  GenerateTerrainMesh();

  // Northern Wastes vegetation clusters
  InitNorthernWastes();

  // Cache obelisk height
  g_obeliskY = GetDuneHeight(OBELISK_X, OBELISK_Z);

  // Camera (2.5D isometric-ish angle)
  float initGroundY = GetDuneHeight(g_player.posX, g_player.posZ);
  g_cam.position = {g_player.posX, initGroundY + 14.0f, g_player.posZ + 10.0f};
  g_cam.target = {g_player.posX, initGroundY, g_player.posZ};
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
  for (int i = 0; i < g_numNpcs; i++) {
    float dx = cx - g_npcs[i].worldX;
    float dz = cz - g_npcs[i].worldZ;
    float minDist = r + 0.4f;
    if (dx * dx + dz * dz < minDist * minDist) return true;
  }
  // Tent AABB collision (mesh-based, no tile marks required)
  for (int i = 0; i < g_numTents; i++) {
    const BoundingBox &bb = g_tentInstances[i].wallAABB;
    float nearX = Clamp(cx, bb.min.x, bb.max.x);
    float nearZ = Clamp(cz, bb.min.z, bb.max.z);
    float dx = cx - nearX, dz = cz - nearZ;
    if (dx * dx + dz * dz < r * r) return true;
  }
  // Rock sphere collision
  for (int i = 0; i < g_numRocks; i++) {
    float dx = cx - g_rockInstances[i].x;
    float dz = cz - g_rockInstances[i].z;
    float minD = r + g_rockInstances[i].scale * 0.9f;
    if (dx * dx + dz * dz < minD * minD) return true;
  }
  // Vegetation sphere collision
  for (int i = 0; i < g_numVeg; i++) {
    float dx = cx - g_vegInstances[i].x;
    float dz = cz - g_vegInstances[i].z;
    float minD = r + g_vegInstances[i].colliderRadius;
    if (dx * dx + dz * dz < minD * minD) return true;
  }
  // Village well (98, 62)
  { float dx = cx - 98.0f, dz = cz - 62.0f;
    float minD = r + 0.9f;
    if (dx*dx + dz*dz < minD*minD) return true; }
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
    for (int i = 0; i < g_numNpcs; i++) {
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
    // Tent AABB push-out — door corridor exempt so player can walk through
    for (int i = 0; i < g_numTents; i++) {
      const TentInstance &ti = g_tentInstances[i];
      const BoundingBox &bb = ti.wallAABB;
      float nearX = Clamp(cx, bb.min.x, bb.max.x);
      float nearZ = Clamp(cz, bb.min.z, bb.max.z);
      float dx = cx - nearX, dz = cz - nearZ;
      float dist2 = dx * dx + dz * dz;
      if (dist2 < r * r && dist2 > 0.0001f) {
        // Check if player is in the door corridor — skip push-out if so
        // Door spans columns doorGX-1 and doorGX (i.e. X from doorGX-1 to doorGX+1),
        // so center = doorGX with ±1.1 tolerance
        float doorCX = (float)ti.doorGX; // actual center of 2-tile-wide door
        bool inDoor = (cx >= doorCX - 1.1f && cx <= doorCX + 1.1f &&
                       cz >= bb.max.z - 1.0f); // south wall approach only
        if (!inDoor) {
          float dist = sqrtf(dist2);
          float push = r - dist + 0.001f;
          cx += (dx / dist) * push;
          cz += (dz / dist) * push;
          resolved = false;
        }
      } else if (dist2 < 0.0001f) {
        // Exactly inside: push south out the door
        float doorCX = (float)ti.doorGX;
        bool inDoor = (cx >= doorCX - 1.1f && cx <= doorCX + 1.1f);
        if (!inDoor) {
          cz += r + 0.05f;
          resolved = false;
        }
      }
    }
    // Rock sphere push-out
    for (int i = 0; i < g_numRocks; i++) {
      float dx = cx - g_rockInstances[i].x;
      float dz = cz - g_rockInstances[i].z;
      float minD = r + g_rockInstances[i].scale * 0.9f;
      float dist2 = dx * dx + dz * dz;
      if (dist2 < minD * minD && dist2 > 0.0001f) {
        float dist = sqrtf(dist2);
        float push = minD - dist + 0.001f;
        cx += (dx / dist) * push;
        cz += (dz / dist) * push;
        resolved = false;
      }
    }
    // Vegetation sphere push-out
    for (int i = 0; i < g_numVeg; i++) {
      float dx = cx - g_vegInstances[i].x;
      float dz = cz - g_vegInstances[i].z;
      float minD = r + g_vegInstances[i].colliderRadius;
      float dist2 = dx * dx + dz * dz;
      if (dist2 < minD * minD && dist2 > 0.0001f) {
        float dist = sqrtf(dist2);
        float push = minD - dist + 0.001f;
        cx += (dx / dist) * push;
        cz += (dz / dist) * push;
        resolved = false;
      }
    }
    // Village well push-out (98, 62)
    { float dx = cx - 98.0f, dz = cz - 62.0f;
      float minD = r + 0.9f;
      float dist2 = dx*dx + dz*dz;
      if (dist2 < minD*minD && dist2 > 0.0001f) {
        float dist = sqrtf(dist2);
        float push = minD - dist + 0.001f;
        cx += (dx/dist) * push;
        cz += (dz/dist) * push;
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
    float speed = PLAYER_SPEED;
    // Swift Boots: hold B or Shift to run at 1.5x
    if (g_inventory.HasSwiftBoots() &&
        (IsKeyDown(KEY_B) || IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)))
      speed *= 1.5f;
    vel = {nx * speed, 0, nz * speed};
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
  for (int i = 0; i < g_numNpcs; i++) {
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
    float gY0 = GetDuneHeight(g_player.posX, g_player.posZ);
    g_cam.position = {g_player.posX, gY0 + 14.0f, g_player.posZ + 10.0f};
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

  // NPC rotation + nomadic movement
  static const float NOMAD_CITY_X[5] = {CITY_ZAHAV_X, CITY_SELA_X, CITY_MAAYAN_X,
                                         CITY_AVAK_X,  CITY_GAN_X};
  static const float NOMAD_CITY_Z[5] = {CITY_ZAHAV_Z, CITY_SELA_Z, CITY_MAAYAN_Z,
                                         CITY_AVAK_Z,  CITY_GAN_Z};
  for (int i = 0; i < g_numNpcs; i++) {
    Dir npcFaceDir = GetNPCPlayerDir({g_npcs[i].worldX, 0, g_npcs[i].worldZ});
    g_npcs[i].targetAngle = DirToAngle(npcFaceDir);
    UpdateFacingAngle(g_npcs[i].facingAngle, g_npcs[i].targetAngle, dt);

    if (!g_npcs[i].nomadic) continue;
    NPC &n = g_npcs[i];
    if (n.waitTimer > 0.0f) {
      n.waitTimer -= dt;
      continue;
    }
    // Move toward destination
    float ddx = n.destX - n.worldX, ddz = n.destZ - n.worldZ;
    float dist = sqrtf(ddx*ddx + ddz*ddz);
    if (dist < 0.5f) {
      // Arrived — wait at city, then pick next city
      n.waitTimer = 30.0f + (float)(GetRandomValue(0, 60));
      // Pick a different city at random
      int newCity = GetRandomValue(0, 4);
      if (fabsf(NOMAD_CITY_X[newCity] - n.destX) < 2.0f)
        newCity = (newCity + 1) % 5;
      n.destX = NOMAD_CITY_X[newCity] + (float)GetRandomValue(-4, 4);
      n.destZ = NOMAD_CITY_Z[newCity] + (float)GetRandomValue(-4, 4);
    } else {
      float speed = 0.45f;
      n.worldX += (ddx / dist) * speed * dt;
      n.worldZ += (ddz / dist) * speed * dt;
      // Face travel direction
      float travelAngle = atan2f(ddx, ddz) * RAD2DEG;
      if (travelAngle < 0) travelAngle += 360.0f;
      n.targetAngle = travelAngle;
    }
  }

  // ── Tent "Ceiling Hide" — handled by UpdateTentVisibility() at frame start.

  // ── NPC interaction: Enter/E key, proximity 1.5 units ──────────────────
  if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_E) || g_padAPressed) &&
      !g_npcDialogOpen && !g_menuOpen) {
    // Find nearest NPC within interaction radius
    float bestDist = 1.5f;
    int bestNPC = -1;
    for (int i = 0; i < g_numNpcs; i++) {
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
  float groundY = GetTerrainBilinear(g_player.posX, g_player.posZ);
  g_cam.position.x = Lerp(g_cam.position.x, g_player.posX, camLerp);
  g_cam.position.z = Lerp(g_cam.position.z, g_player.posZ + 10.0f, camLerp);
  g_cam.position.y = Lerp(g_cam.position.y, groundY + 14.0f, camLerp);
  g_cam.target.x = Lerp(g_cam.target.x, g_player.posX, camLerp);
  g_cam.target.z = Lerp(g_cam.target.z, g_player.posZ, camLerp);
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
  float gy = basePos.y + 0.012f;
  // Warm brownish-amber shadow — blends naturally with desert sand
  DrawCylinder({basePos.x, gy, basePos.z}, radius * 1.05f, radius * 1.05f, 0.008f,
               8, {45, 28, 8, 78});
  DrawCylinder({basePos.x, gy + 0.001f, basePos.z}, radius * 0.72f, radius * 0.72f,
               0.008f, 8, {35, 20, 5, 48});
}

// ═══════════════════════════════════════════════════════════════════════════
// §8  FIVE CITIES OF ERETZ
// ═══════════════════════════════════════════════════════════════════════════

// Helpers shared across city draw functions
static void DrawGoldBuilding(Vector3 center, float sx, float sy, float sz) {
  if (!g_cityModelsReady) return;
  SetShaderValue(g_shGold, g_locGoldTime,   &g_time,          SHADER_UNIFORM_FLOAT);
  Vector3 cp = g_cam.position;
  SetShaderValue(g_shGold, g_locGoldCamPos, &cp,              SHADER_UNIFORM_VEC3);
  Vector3 sd = g_sunDir;
  SetShaderValue(g_shGold, g_locGoldSunDir, &sd,              SHADER_UNIFORM_VEC3);
  DrawModelEx(g_goldModel, center, {0,1,0}, 0.0f, {sx, sy, sz}, WHITE);
}

static void DrawWaterPlane(Vector3 center, float w, float d) {
  if (!g_cityModelsReady) return;
  SetShaderValue(g_shWater, g_locWaterTime, &g_time, SHADER_UNIFORM_FLOAT);
  DrawModelEx(g_waterModel, center, {0,1,0}, 0.0f, {w, 1.0f, d}, WHITE);
}


// ─── 2. Ma'ayan — The Oasis (W, ~22,75) ──────────────────────────────────────
static void DrawCityMaayan() {
  const float cx = CITY_MAAYAN_X, cz = CITY_MAAYAN_Z;
  const float poolY = GetDuneHeight(cx, cz);
  const Color limestone = {215, 208, 188, 255};
  const Color cedar     = {165, 108, 48,  255};
  const Color waterRim  = {200, 192, 172, 255};

  // Pool rim (4 stone slabs)
  DrawCube({cx,        poolY + 0.2f, cz - 3.3f}, 7.0f, 0.4f, 0.6f, waterRim);
  DrawCube({cx,        poolY + 0.2f, cz + 3.3f}, 7.0f, 0.4f, 0.6f, waterRim);
  DrawCube({cx - 3.3f, poolY + 0.2f, cz},        0.6f, 0.4f, 7.0f, waterRim);
  DrawCube({cx + 3.3f, poolY + 0.2f, cz},        0.6f, 0.4f, 7.0f, waterRim);
  // Water surface (shader-driven)
  DrawWaterPlane({cx, poolY + 0.1f, cz}, 6.0f, 6.0f);
  // Fallback solid pool for when shader is loading
  DrawCylinder({cx, poolY + 0.08f, cz}, 2.9f, 2.9f, 0.04f, 16,
               {60, 140, 200, 180});
  // 8-pillar colonnade ring
  for (int i = 0; i < 8; i++) {
    float a  = i * (2.0f * PI / 8.0f);
    float px = cx + cosf(a) * 6.8f;
    float pz = cz + sinf(a) * 6.8f;
    float py = GetDuneHeight(px, pz);
    DrawCylinder({px, py, pz}, 0.26f, 0.22f, 4.2f, 10, limestone);
    DrawCube({px, py + 4.3f, pz}, 0.65f, 0.48f, 0.65f, limestone); // capital
  }
  // Aqueduct arches: N-S span
  {
    float aqY = GetDuneHeight(cx, cz) + 5.5f;
    DrawCube({cx, aqY, cz},        0.45f, 0.45f, 18.0f, {185, 172, 145, 255});
    DrawCylinder({cx, GetDuneHeight(cx, cz - 9.0f), cz - 9.0f},
                 0.28f, 0.28f, aqY - GetDuneHeight(cx, cz - 9.0f),
                 6, {180, 168, 140, 255});
    DrawCylinder({cx, GetDuneHeight(cx, cz + 9.0f), cz + 9.0f},
                 0.28f, 0.28f, aqY - GetDuneHeight(cx, cz + 9.0f),
                 6, {180, 168, 140, 255});
  }
  // Aqueduct arches: E-W span
  {
    float aqY = GetDuneHeight(cx, cz) + 5.5f;
    DrawCube({cx, aqY, cz},        18.0f, 0.45f, 0.45f, {185, 172, 145, 255});
    DrawCylinder({cx - 9.0f, GetDuneHeight(cx - 9.0f, cz), cz},
                 0.28f, 0.28f, aqY - GetDuneHeight(cx - 9.0f, cz),
                 6, {180, 168, 140, 255});
    DrawCylinder({cx + 9.0f, GetDuneHeight(cx + 9.0f, cz), cz},
                 0.28f, 0.28f, aqY - GetDuneHeight(cx + 9.0f, cz),
                 6, {180, 168, 140, 255});
  }
  // 4 courtyard houses with cedar pergola
  static const float hdx[] = {-5.5f,  5.5f, 0.0f,  0.0f};
  static const float hdz[] = {  0.0f,  0.0f,-5.5f,  5.5f};
  for (int i = 0; i < 4; i++) {
    float hx = cx + hdx[i], hz = cz + hdz[i];
    float hy = GetDuneHeight(hx, hz);
    DrawCube({hx, hy + 1.5f, hz}, 2.8f, 3.0f, 2.8f, {222, 216, 196, 255});
    DrawCube({hx, hy + 3.2f, hz}, 3.1f, 0.32f, 3.1f, cedar); // cedar roof
  }
}

// ─── 3. Avak — The Dust Town (S, ~100,133) ───────────────────────────────────
static void DrawCityAvak() {
  const float cx = CITY_AVAK_X, cz = CITY_AVAK_Z;
  const Color eroded  = {192, 182, 155, 255};
  const Color bleach  = {205, 197, 172, 255};
  const Color rubbleC = {172, 162, 136, 255};

  // Crumbling wall segments (partial, jagged heights)
  struct WallSeg { float dx, dz, w, h, d; };
  static const WallSeg walls[] = {
    {-6.0f, -5.5f,  8.5f, 2.6f, 0.75f},
    { 5.5f, -2.5f,  0.7f, 3.0f, 5.5f },
    {-5.5f,  4.5f,  5.0f, 1.8f, 0.7f },
    { 1.0f,  5.8f,  4.5f, 2.2f, 0.65f},
    {-1.5f, -7.0f,  3.5f, 1.4f, 0.6f },
  };
  for (auto &w : walls) {
    float wx = cx + w.dx, wz = cz + w.dz;
    float wy = GetDuneHeight(wx, wz);
    DrawCube({wx, wy + w.h*0.5f, wz}, w.w, w.h, w.d, eroded);
    DrawCubeWires({wx, wy + w.h*0.5f, wz}, w.w, w.h, w.d, {140, 130, 108, 200});
  }
  // Rubble piles
  struct Rubble { float dx, dz, sc; };
  static const Rubble rubble[] = {
    {-4.0f, -7.0f, 1.3f}, { 3.8f, -6.2f, 0.9f},
    {-7.0f,  2.5f, 1.1f}, { 4.5f,  4.0f, 1.4f},
    { 1.5f,  5.5f, 0.9f}, {-2.0f,  1.5f, 0.7f},
  };
  for (auto &r : rubble) {
    float rx = cx + r.dx, rz = cz + r.dz;
    float ry = GetDuneHeight(rx, rz);
    DrawSphere({rx,            ry + r.sc*0.30f, rz},            r.sc*0.55f, rubbleC);
    DrawSphere({rx + r.sc*0.3f, ry + r.sc*0.15f, rz + r.sc*0.2f}, r.sc*0.42f, bleach);
  }
  // ── Broken Clock Tower (navigation landmark) ──────────────────────────────
  float ty = GetDuneHeight(cx + 1.5f, cz - 2.5f);
  // Main shaft
  DrawCylinder({cx+1.5f, ty, cz-2.5f}, 1.25f, 1.25f, 5.8f, 10, eroded);
  // Sheared top (diagonal suggestion: taper to one side)
  DrawCylinder({cx+1.5f, ty+5.8f, cz-2.5f}, 1.25f, 0.45f, 1.4f, 8, bleach);
  // Clock face inlay
  DrawCylinder({cx+2.75f, ty+4.2f, cz-2.5f}, 0.7f, 0.7f, 0.08f, 14,
               {148, 138, 115, 255});
  // Crack line (thin dark cube along tower face)
  DrawCube({cx+2.6f, ty+3.5f, cz-2.5f}, 0.07f, 4.5f, 0.07f, {85, 75, 60, 255});
  // Narrow alley walls
  DrawCube({cx-2.0f, GetDuneHeight(cx-2.0f,cz+1.5f)+1.3f, cz+1.5f},
           0.6f, 2.6f, 5.5f, {186, 176, 150, 255});
  DrawCube({cx+3.0f, GetDuneHeight(cx+3.0f,cz+1.5f)+1.1f, cz+1.5f},
           0.6f, 2.2f, 5.5f, {180, 170, 144, 255});
}

// ─── 4. Gan — The Blooming Town (E, ~177,75) ─────────────────────────────────
static void DrawCityGan() {
  const float cx = CITY_GAN_X, cz = CITY_GAN_Z;
  const Color fittedStone = {180, 162, 122, 255};
  const Color cedar       = {162, 108, 46,  255};
  const Color channel     = {80,  130, 175, 180};

  // 5 terrace retaining walls (concentric)
  for (int tier = 0; tier < 5; tier++) {
    float radius  = (float)(tier + 1) * 3.2f;
    float wallH   = 0.9f + tier * 0.1f;
    int   sides   = 8 + tier * 2;
    float segLen  = 2.0f * PI * radius / sides;
    for (int i = 0; i < sides; i++) {
      float a  = i * (2.0f * PI / sides);
      float wx = cx + cosf(a) * radius;
      float wz = cz + sinf(a) * radius;
      float wy = GetDuneHeight(wx, wz);
      DrawCube({wx, wy + wallH*0.5f, wz}, segLen * 0.88f, wallH, 0.55f, fittedStone);
    }
  }
  // Cedar-framed residential houses (5 positions)
  static const float hdx[] = {-5.0f,  5.0f, 0.0f, -8.0f,  8.0f};
  static const float hdz[] = {-4.0f, -4.0f,-8.0f,  0.5f,  0.5f};
  for (int i = 0; i < 5; i++) {
    float hx = cx + hdx[i], hz = cz + hdz[i];
    float hy = GetDuneHeight(hx, hz);
    DrawCube({hx, hy + 1.25f, hz}, 2.5f, 2.5f, 2.5f, {215, 205, 178, 255});
    DrawCube({hx, hy + 2.65f, hz}, 2.75f, 0.35f, 2.75f, cedar);
  }
  // Irrigation channels (N-S lines, thin water-blue slabs)
  for (int i = -2; i <= 2; i++) {
    float lx = cx + i * 3.1f;
    float ly = GetDuneHeight(lx, cz);
    DrawCube({lx, ly + 0.025f, cz}, 0.28f, 0.05f, 28.0f, channel);
  }
  // Water tower (summit landmark)
  float wy = GetDuneHeight(cx, cz);
  DrawCylinder({cx, wy,       cz}, 1.05f, 1.25f, 6.2f, 10, {192, 178, 142, 255});
  DrawCylinder({cx, wy + 6.2f, cz}, 1.9f, 1.7f,  1.3f, 10, {200, 185, 150, 255});
  for (int i = 0; i < 4; i++) {
    float sa = i * (PI / 2.0f) + PI / 4.0f;
    float sx = cx + cosf(sa) * 1.6f, sz = cz + sinf(sa) * 1.6f;
    float sy = GetDuneHeight(sx, sz);
    DrawCylinder({sx, sy + 0.5f, sz}, 0.12f, 0.08f, 5.8f, 5, cedar);
  }
  // Dense vegetation patches between terraces
  for (int v = 0; v < 12; v++) {
    float va = v * (2.0f * PI / 12.0f);
    float vr = 2.0f + (v % 3) * 1.8f;
    float vx = cx + cosf(va) * vr, vz = cz + sinf(va) * vr;
    float vy = GetDuneHeight(vx, vz);
    // Stylised plant: small green sphere on thin stem
    DrawCylinder({vx, vy, vz}, 0.06f, 0.06f, 0.55f, 4, {90, 65, 35, 255});
    Color leaf = {(unsigned char)(60 + v*8%60), (unsigned char)(140 + v*5%60),
                  (unsigned char)(50 + v*3%40), 255};
    DrawSphere({vx, vy + 0.75f, vz}, 0.38f + 0.1f*(v%3), leaf);
  }
}

// ─── 5. Sela — The Canyon Town (NW, ~35,30) ──────────────────────────────────
static void DrawCitySela() {
  const float cx = CITY_SELA_X, cz = CITY_SELA_Z;
  const float floorY = GetDuneHeight(cx, cz);
  const Color rawStone  = {118, 104,  84, 255};
  const Color stoneFace = {135, 120,  98, 255};
  const Color brass     = {175, 112,  40, 255};
  const Color brassDark = {122,  76,  24, 255};

  // ── Canyon wall faces (N and S cliff faces) ──────────────────────────────
  for (int side = 0; side < 2; side++) {
    float wallZ    = cz + (side == 0 ? -7.5f : 7.5f);
    int   numSegs  = 10;
    for (int i = -4; i <= 4; i++) {
      float wx    = cx + i * 2.5f;
      float wrimY = GetDuneHeight(wx, wallZ + (side==0 ? -2.0f : 2.0f));
      float wflrY = GetDuneHeight(wx, cz);
      float wallH = wrimY - wflrY;
      if (wallH < 0.5f) wallH = 0.5f;
      DrawCube({wx, wflrY + wallH*0.5f, wallZ}, 2.3f, wallH, 1.1f, rawStone);
    }
  }
  // ── Rock-cut dwellings (N wall) ──────────────────────────────────────────
  for (int i = 0; i < 6; i++) {
    float dx    = (i - 2.5f) * 3.5f;
    float tier  = (float)(i % 2);
    float dwx   = cx + dx;
    float dwz   = cz - 6.0f;
    float dwy   = floorY + 1.8f + tier * 2.8f;
    DrawCube({dwx, dwy + 0.9f, dwz}, 1.9f, 2.0f, 0.9f, {78, 68, 54, 255});
    DrawCubeWires({dwx, dwy + 0.9f, dwz}, 2.2f, 2.3f, 1.0f, stoneFace);
  }
  // ── Rock-cut dwellings (S wall) ──────────────────────────────────────────
  for (int i = 0; i < 5; i++) {
    float dx  = (i - 2.0f) * 3.8f;
    float dwx = cx + dx;
    float dwz = cz + 6.5f;
    float dwy = floorY + 1.4f + (i % 3) * 2.4f;
    DrawCube({dwx, dwy + 0.8f, dwz}, 2.0f, 1.8f, 0.9f, {73, 63, 50, 255});
    DrawCubeWires({dwx, dwy + 0.8f, dwz}, 2.3f, 2.1f, 1.0f, stoneFace);
  }
  // ── 3-level brass scaffolding (both faces) ───────────────────────────────
  for (int level = 0; level < 3; level++) {
    float scaffY = floorY + 1.0f + level * 3.0f;
    DrawCube({cx, scaffY, cz - 5.5f}, 20.0f, 0.12f, 0.12f, brass);
    DrawCube({cx, scaffY, cz + 5.5f}, 20.0f, 0.12f, 0.12f, brass);
    for (int p = -4; p <= 4; p++) {
      float px = cx + p * 2.3f;
      float postH = scaffY - floorY + 0.15f;
      DrawCylinder({px, floorY, cz - 5.5f}, 0.09f, 0.09f, postH, 5, brassDark);
      DrawCylinder({px, floorY, cz + 5.5f}, 0.09f, 0.09f, postH, 5, brassDark);
    }
  }
  // ── Great Chain Lift (navigation landmark) ────────────────────────────────
  float rimY    = GetDuneHeight(cx + 2.0f, cz - 10.0f);
  float chainY0 = floorY;
  int   nLinks  = (int)((rimY - chainY0) / 1.5f) + 2;
  for (int lk = 0; lk < nLinks; lk++) {
    float ly = chainY0 + lk * 1.5f;
    if (lk % 2 == 0)
      DrawCube({cx + 2.0f, ly, cz}, 0.5f, 0.18f, 0.12f, brass);
    else
      DrawCube({cx + 2.0f, ly, cz}, 0.12f, 0.18f, 0.5f, brass);
    // Ore cart mid-chain
    if (lk == nLinks / 2) {
      float ct = g_time * 0.3f;
      float cartY = chainY0 + ct - floorf(ct) * (rimY - chainY0);
      DrawCube({cx+2.0f, cartY - 0.6f, cz}, 1.2f, 0.7f, 0.8f, {88, 78, 62, 255});
    }
  }
  // ── Brass lanterns along scaffold rails ──────────────────────────────────
  struct Lantern { float dx, dz, dy; };
  static const Lantern lanterns[] = {
    {-7.5f, -5.5f, 2.5f}, {-3.8f, -5.5f, 3.8f}, {0.0f, -5.5f, 2.5f},
    { 3.8f, -5.5f, 3.8f}, { 7.5f, -5.5f, 2.5f},
    {-5.0f,  5.5f, 2.0f}, { 0.0f,  5.5f, 3.4f}, { 5.0f,  5.5f, 2.0f},
  };
  for (auto &l : lanterns) {
    float lx = cx + l.dx, lz = cz + l.dz, ly = floorY + l.dy;
    float glow = 0.5f + 0.5f * sinf(g_time * 2.1f + l.dx * 0.7f);
    unsigned char cr = 255;
    unsigned char cg = (unsigned char)(130 + glow * 85);
    DrawSphere({lx, ly,       lz}, 0.22f,        {cr, cg, 22, 255});
    DrawSphere({lx, ly,       lz}, 0.40f,        {255, 185, 50, (unsigned char)(32 + glow*28)});
    DrawCube  ({lx, ly - 0.1f, lz}, 0.42f, 0.5f, 0.42f, {115, 76, 22, 140});
  }
  // ── Rope bridges ─────────────────────────────────────────────────────────
  for (int b = 0; b < 2; b++) {
    float by  = floorY + 3.2f + b * 4.0f;
    float bxc = cx + (b == 0 ? -2.5f : 1.5f);
    DrawCube({bxc, by,        cz}, 1.1f, 0.09f, 14.0f, {175, 150, 88, 255});
    DrawCylinder({bxc, by+0.9f, cz - 7.0f}, 0.06f, 0.06f, 8.0f, 4, {140, 110, 60, 255});
    DrawCylinder({bxc, by+0.9f, cz + 7.0f}, 0.06f, 0.06f, 8.0f, 4, {140, 110, 60, 255});
  }
}

// ── Village Well at (98, 62) ─────────────────────────────────────────────────
static void DrawVillageWell() {
  float wx = 98.0f, wz = 62.0f;
  float wy = GetDuneHeight(wx, wz);
  // Stone base ring
  DrawCylinder({wx, wy, wz}, 0.9f, 0.85f, 0.6f, 12, {130, 120, 105, 255});
  DrawCylinder({wx, wy, wz}, 0.92f, 0.92f, 0.62f, 12, {90, 80, 65, 160}); // rim shadow
  // Water surface inside
  DrawCylinder({wx, wy + 0.55f, wz}, 0.72f, 0.72f, 0.02f, 12, {60, 120, 180, 200});
  // Two vertical posts
  DrawCylinder({wx - 0.6f, wy + 0.6f, wz}, 0.06f, 0.06f, 1.2f, 6, {100, 75, 45, 255});
  DrawCylinder({wx + 0.6f, wy + 0.6f, wz}, 0.06f, 0.06f, 1.2f, 6, {100, 75, 45, 255});
  // Crossbeam
  DrawCube({wx, wy + 1.85f, wz}, 1.35f, 0.1f, 0.1f, {100, 75, 45, 255});
  // Rope (thin cylinder)
  DrawCylinder({wx, wy + 0.6f, wz}, 0.025f, 0.025f, 1.25f, 5, {160, 130, 80, 255});
  // Bucket at bottom of rope
  DrawCylinder({wx, wy + 0.55f, wz}, 0.12f, 0.10f, 0.18f, 8, {110, 80, 45, 255});
}

// Dark cel-outline colors — used for the 1.12x backface inflation outline pass
static ChibiColors g_celOutline;
static bool        g_celOutlineReady = false;

static void DrawOverworld() {
  // ── Render 3D scene into FBO for post-processing ────────────────────────
  BeginTextureMode(g_sceneFBO);
  // ── Cel outline colors (lazy init) ──────────────────────────────────────
  if (!g_celOutlineReady) {
    Color dark = {18, 12, 6, 255};
    g_celOutline = MakeChibiColors(dark, dark, dark, dark, dark, dark);
    g_celOutlineReady = true;
  }

  // Day/night ambient tint
  Color ambBase = g_worldClock.GetAmbientTint();
  Color ambWarm = {220, 180, 140, 255};
  Color amb = {(unsigned char)Clamp(ambBase.r * 0.6f + ambWarm.r * 0.4f, 0, 255),
               (unsigned char)Clamp(ambBase.g * 0.6f + ambWarm.g * 0.4f, 0, 255),
               (unsigned char)Clamp(ambBase.b * 0.6f + ambWarm.b * 0.4f, 0, 255),
               255};

  // Sky color shifts with time of day
  ClearBackground(g_worldClock.GetSkyColor());

  BeginMode3D(g_cam);
  {
    UpdateFrustumPlanes(); // Gribb-Hartmann frustum extraction for culling
    UpdateSunDir();        // Recompute shadow direction from time of day

    // Update triplanar shader view-dependent uniforms (rim light needs camera)
    Vector3 camPos = g_cam.position;
    SetShaderValue(g_shTriplanar, g_locTripCameraPos, &camPos, SHADER_UNIFORM_VEC3);
    SetShaderValue(g_shTriplanar, g_locTripSunDir,    &g_sunDir, SHADER_UNIFORM_VEC3);
    // ── PASS 1: Opaque terrain (dune heightmap mesh) ─────────────────────
    if (g_terrainReady) {
      DrawModel(g_terrainModel, {0, 0, 0}, 1.0f, amb);
    }

    // ── PASS 2: Opaque environment (tents, rocks) ───────────────────────
    // Tent shadows (PCF soft) — cull beyond 50 units
    for (int i = 0; i < g_numTents; i++) {
      const TentInstance &ti = g_tentInstances[i];
      if (DistSqToPlayer(ti.worldX, ti.worldZ) > 2500.0f) continue;
      DrawTentShadow(ti);
    }

    // Character shadows
    float playerY = GetDuneHeight(g_player.posX, g_player.posZ);
    DrawSpriteBlobShadow({g_player.posX, playerY, g_player.posZ}, 0.5f);
    for (int i = 0; i < g_numNpcs; i++) {
      if (DistSqToPlayer(g_npcs[i].worldX, g_npcs[i].worldZ) > 1225.0f) continue; // 35u
      float ny = GetDuneHeight(g_npcs[i].worldX, g_npcs[i].worldZ);
      DrawSpriteBlobShadow({g_npcs[i].worldX, ny, g_npcs[i].worldZ}, 0.42f);
    }

    // Rock shadows — cull beyond 40 units
    for (int i = 0; i < g_numRocks; i++) {
      RockInstance &ri = g_rockInstances[i];
      if (DistSqToPlayer(ri.x, ri.z) > 1600.0f) continue;
      DrawSoftShadow({ri.x, ri.cachedY, ri.z}, ri.scale * 1.5f, ri.scale * 2.0f);
    }

    // Draw tents — cull beyond 50 units
    for (int i = 0; i < g_numTents; i++) {
      const TentInstance &ti = g_tentInstances[i];
      if (DistSqToPlayer(ti.worldX, ti.worldZ) > 2500.0f) continue;
      DrawTent(ti);
    }

    // Village well
    DrawVillageWell();

    // ── Five Cities of Eretz (frustum + distance culled) ─────────────────
    if (SphereInFrustum( 22.0f,  3.0f,  75.0f, 14.0f)) DrawCityMaayan();
    if (SphereInFrustum(100.0f,  1.0f, 133.0f, 12.0f)) DrawCityAvak();
    if (SphereInFrustum(177.0f,  4.0f,  75.0f, 16.0f)) DrawCityGan();
    if (SphereInFrustum( 35.0f, -1.0f,  30.0f, 13.0f)) DrawCitySela();

    // Draw procedural rocks — cull beyond 40 units + frustum
    for (int i = 0; i < g_numRocks; i++) {
      RockInstance &ri = g_rockInstances[i];
      if (DistSqToPlayer(ri.x, ri.z) > 1600.0f) continue;
      if (!SphereInFrustum(ri.x, ri.cachedY, ri.z, ri.scale * 1.5f)) continue;
      DrawModelEx(ri.model, {ri.x, ri.cachedY, ri.z}, {0, 1, 0}, ri.rotY,
                  {ri.scale, ri.scale, ri.scale}, WHITE);
    }


    // ── PASS 2b: Northern Wastes vegetation & obelisk ─────────────────────
    // Vegetation shadows — cull beyond 40 units
    for (int i = 0; i < g_numVeg; i++) {
      const VegetationInstance &vi = g_vegInstances[i];
      if (DistSqToPlayer(vi.x, vi.z) > 1600.0f) continue;
      float shadowH = (vi.type == VEG_SAGUARO || vi.type == VEG_DRY_TREE)
                      ? 3.0f * vi.scale : 1.2f * vi.scale;
      DrawSoftShadow({vi.x, vi.cachedY, vi.z}, vi.scale * 0.8f, shadowH);
    }
    // Obelisk shadow
    DrawSoftShadow({OBELISK_X, g_obeliskY, OBELISK_Z}, 0.6f, 2.5f);

    // Vegetation models — cull beyond 40 units + frustum
    for (int i = 0; i < g_numVeg; i++) {
      const VegetationInstance &vi = g_vegInstances[i];
      if (DistSqToPlayer(vi.x, vi.z) > 1600.0f) continue;
      if (!SphereInFrustum(vi.x, vi.cachedY, vi.z, vi.scale * 2.5f)) continue;
      DrawVegetationInstance(vi);
    }

    // Ancient obelisk (northern point of interest)
    DrawNorthernObelisk();

    // ── PASS 3: 3D Chibi Characters — cel outline + fill ─────────────────
    // Outline: draw at 1.12× scale with dark colour before the normal pass.
    // The larger backface silhouette that sticks out around the normal model
    // creates a crisp cartoon "ink line" outline effect.
    auto DrawChibiWithOutline = [&](Vector3 pos, float angle,
                                    const ChibiColors &col, float phase,
                                    bool walking, float rim) {
      rlPushMatrix();
        rlTranslatef(pos.x, pos.y, pos.z);
        rlScalef(1.13f, 1.13f, 1.13f);
        rlTranslatef(-pos.x, -pos.y, -pos.z);
        DrawChibiModel3D(pos, angle, g_celOutline, phase, walking, 0.0f);
      rlPopMatrix();
      DrawChibiModel3D(pos, angle, col, phase, walking, rim);
    };

    // NPCs — distance cull only (frustum skipped: NPCs are always upright,
    // passing Y=0 in SphereInFrustum would falsely cull elevated terrain NPCs)
    for (int i = 0; i < g_numNpcs; i++) {
      float dsq = DistSqToPlayer(g_npcs[i].worldX, g_npcs[i].worldZ);
      if (dsq > 1225.0f) continue; // 35 unit hard cull
      float npcY = GetTerrainBilinear(g_npcs[i].worldX, g_npcs[i].worldZ) + 0.06f;
      Vector3 npcPos = {g_npcs[i].worldX, npcY, g_npcs[i].worldZ};
      if (dsq < 100.0f) // 10 units: full outline pass
        DrawChibiWithOutline(npcPos, g_npcs[i].facingAngle, g_npcs[i].colors,
                             g_time + i * 0.22f, false, 0.6f);
      else
        DrawChibiModel3D(npcPos, g_npcs[i].facingAngle, g_npcs[i].colors,
                         g_time + i * 0.22f, false, 0.6f);
    }

    // Player — multi-point terrain sample + citadel platform snap
    {
      // Sample 4 cardinal points around the player for terrain snapping
      float py = GetTerrainBilinear(g_player.posX, g_player.posZ);
      py = fmaxf(py, GetTerrainBilinear(g_player.posX + 0.25f, g_player.posZ));
      py = fmaxf(py, GetTerrainBilinear(g_player.posX - 0.25f, g_player.posZ));
      py = fmaxf(py, GetTerrainBilinear(g_player.posX, g_player.posZ + 0.25f));
      py = fmaxf(py, GetTerrainBilinear(g_player.posX, g_player.posZ - 0.25f));
      py += 0.08f; // generous offset — bilinear can underestimate the mesh surface
      Vector3 playerPos = {g_player.posX, py, g_player.posZ};
      DrawChibiWithOutline(playerPos, g_player.facingAngle, g_playerColors,
                           g_player.animTimer, g_player.moving, 0.8f);
    }
  }
  EndMode3D();
  EndTextureMode();

  // ── Full post-processing (composite renders into g_pixelFBO) ───────────
  ApplyFullPostProcess(g_time);

  // ── Night mode: dark vignette + star field ────────────────────────────────
  if (g_worldClock.IsNight()) {
    float nd = (g_worldClock.GetPhaseNorm() - 0.667f) / 0.333f; // 0-1 night progress
    // Darkness ramp: fade in at dusk, hold, fade out at dawn
    float dark = sinf(nd * PI); // peak at midnight
    unsigned char nightA = (unsigned char)(dark * 145);
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, {8, 12, 30, nightA});

    // Static star field (seeded positions)
    srand(42);
    for (int i = 0; i < 80; i++) {
      int sx = rand() % SCREEN_W;
      int sy = rand() % (SCREEN_H / 2);
      float twinkle = sinf(g_time * (2.0f + i * 0.07f) + i * 1.3f) * 0.5f + 0.5f;
      unsigned char starA = (unsigned char)(dark * (80 + twinkle * 120));
      int sr = (i % 3 == 0) ? 2 : 1;
      DrawCircle(sx, sy, sr, {230, 235, 255, starA});
    }
    srand((unsigned int)g_time); // restore random state
  }
  // Evening glow (warm sunset tint fading in as day ends)
  else {
    float t = g_worldClock.GetPhaseNorm();
    if (t >= 0.333f) {
      float ev = (t - 0.333f) / 0.334f; // 0-1 evening progress
      float evFade = sinf(ev * PI * 0.5f); // ramp up toward night
      unsigned char evA = (unsigned char)(evFade * 38);
      DrawRectangle(0, 0, SCREEN_W, SCREEN_H, {180, 75, 20, evA});
    }
  }

  // ── Southern sandstorm overlay (amber screen tint) ─────────────────────
  {
    float halfH = (float)(MAP_H / 2);
    float southP = Clamp((g_player.posZ - halfH) / (halfH * 0.8f), 0.0f, 1.0f);
    if (southP > 0.0f) {
      // Base amber tint
      Color sand = {210, 150, 60, (unsigned char)(southP * 100)};
      DrawRectangle(0, 0, SCREEN_W, SCREEN_H, sand);
      // Scanline effect: horizontal dust streaks
      float streak = sinf(g_time * 4.2f) * 0.5f + 0.5f;
      Color streak1 = {240, 180, 80, (unsigned char)(southP * streak * 40)};
      Color streak2 = {0, 0, 0, 0};
      DrawRectangleGradientV(0, SCREEN_H / 3, SCREEN_W, SCREEN_H / 6,
                             streak2, streak1);
      DrawRectangleGradientV(0, SCREEN_H / 2, SCREEN_W, SCREEN_H / 4,
                             streak1, streak2);
    }
  }

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

  // ── City proximity banner ─────────────────────────────────────────────────
  {
    float nearDist = CITY_RADIUS * 1.5f;
    int   nearCity = -1;
    float bestD2   = nearDist * nearDist;
    for (int ci = 0; ci < 5; ci++) {
      float dx = g_player.posX - CITY_POS[ci][0];
      float dz = g_player.posZ - CITY_POS[ci][1];
      float d2 = dx*dx + dz*dz;
      if (d2 < bestD2) { bestD2 = d2; nearCity = ci; }
    }
    if (nearCity >= 0 && CITY_NAMES[nearCity][0] != '\0') {
      float fade = 1.0f - sqrtf(bestD2) / nearDist;
      unsigned char alpha = (unsigned char)(Clamp(fade * 340.0f, 0, 220));
      // City name banner (centered, top of screen)
      const char *cn = CITY_NAMES[nearCity];
      int tw = MeasureText(cn, 18);
      DrawRectangle((SCREEN_W - tw - 24) / 2, 68, tw + 24, 28,
                    {15, 10, 5, (unsigned char)(alpha * 3 / 5)});
      DrawText(cn, (SCREEN_W - tw) / 2, 75, 18, {255, 220, 120, alpha});
    }
  }
  // City badges row — small colored circles per city, filled if badge earned
  {
    const char *cityInitials[] = {"Z","M","A","G","S"};
    const Color cityColors[]   = {{220,180,60,255},{60,200,160,255},{200,120,60,255},
                                   {80,200,80,255},{200,80,80,255}};
    int bx = 10, by = 44;
    DrawText("Badges:", bx, by, 10, {180,170,140,180});
    bx += 52;
    for (int i = 0; i < 5; i++) {
      bool earned = g_tournament.cityChampionDefeated[i];
      Color bg  = earned ? cityColors[i] : Color{40,30,20,120};
      Color rim = earned ? Color{255,240,160,220} : Color{120,100,60,120};
      DrawCircle(bx + 8, by + 5, 8.0f, bg);
      DrawCircleLines(bx + 8, by + 5, 8.0f, rim);
      DrawText(cityInitials[i], bx + 4, by + 0, 10,
               earned ? Color{30,20,10,255} : Color{120,100,60,160});
      bx += 22;
    }
    // If currently in a tournament, show round progress
    if (g_tournamentMode || g_tournament.roundsWon > 0) {
      snprintf(hudBuf, 128, "  Round %d/3", g_tournament.roundsWon);
      DrawText(hudBuf, bx + 4, by, 10, {220,200,100,200});
    }
  }

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
  InitCardShaders();
  InitOverworld();
  InitCharacterColors(); // 3D chibi color palettes (no more sprite textures)
  g_market.Init();
  g_tournament.Init();
  g_worldClock.Init();
  g_inventory.Init();
  InitSounds();

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
  UnloadCardShaders();
  CloseSounds();
  CloseWindow();
  return 0;
}
