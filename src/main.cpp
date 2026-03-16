// ═══════════════════════════════════════════════════════════════════════════════
// "Sovereign Unova" — 2.5D Desert Village Engine  (Pokémon BW style)
// Raylib 5.5 · C++17 · Hybrid 3D terrain + 2D billboard sprites
// POST-PROCESSING: Bloom, God Rays, Heat Haze, DoF, Color Grading, Vignette
// ═══════════════════════════════════════════════════════════════════════════════
#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"
#include "imgui.h"
#include "rlImGui.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <cstdlib>
#include <ctime>

// ═══════════════════════════════════════════════════════════════════════════════
// GLSL 330 SHADER SOURCES
// ═══════════════════════════════════════════════════════════════════════════════
static const char* FS_BLOOM_EXTRACT = R"(
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

static const char* FS_BLUR = R"(
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

static const char* FS_GODRAYS = R"(
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

// Final composite: heat haze + DoF + bloom + godrays + #FFF4D6 sun + color LUT + vibrance
static const char* FS_COMPOSITE = R"(
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
    float blurR=smoothstep(0.10,0.45,dofDist)*3.0/resolution.y;
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
static const char* FS_SANDSTORM = R"(
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

// ── HD-2D Sprite Lighting shader (directional sun on pixel-art billboard) ────
static const char* VS_SPRITE_LIT = R"(
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

static const char* FS_SPRITE_LIT = R"(
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

// ── Post-processing state ────────────────────────────────────────────────────
static RenderTexture2D g_sceneFBO, g_brightFBO, g_blurA, g_blurB, g_godrayFBO;
static Shader g_shBloomExtract, g_shBlur, g_shGodrays, g_shComposite, g_shSandstorm;
static Shader g_shSpriteLit;
static int g_locSpriteSunLR;
static int g_locThreshold, g_locBlurDir, g_locSunPos;
static int g_locBloomTex, g_locGodrayTex, g_locBloomStr, g_locGodrayStr;
static int g_locStormTime, g_locStormRes;
static int g_locCompTime, g_locCompExposure, g_locCompRes;

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr int   SCREEN_W   = 960;
static constexpr int   SCREEN_H   = 640;
static constexpr int   MAP_W      = 40;
static constexpr int   MAP_H      = 30;
static constexpr int   INT_W      = 10;
static constexpr int   INT_H      = 8;
static constexpr float TILE_W     = 1.0f;    // 1 world unit = 1 tile
static constexpr float MOVE_SPEED = 4.0f;
static constexpr int   SPR_W      = 24;
static constexpr int   SPR_H      = 32;
static constexpr float SPR_SCALE  = 1.0f;    // player fits 1×1 tile
static constexpr int   MAX_PARTICLES = 120;
static constexpr int   TENT_SIZE  = 4;       // tents are 4×4 tiles
static constexpr float EXPOSURE   = 1.45f;   // overexposed desert sun
// Sun direction for shadow projection (from top-right)
static constexpr float SUN_DX     = -0.5f;
static constexpr float SUN_DZ     = 0.35f;

enum Dir { DIR_DOWN=0, DIR_UP, DIR_LEFT, DIR_RIGHT, DIR_COUNT };
static constexpr int FRAMES_PER_DIR = 3;
static constexpr int TOTAL_FRAMES   = DIR_COUNT * FRAMES_PER_DIR;

// ── Post-processing init/cleanup ─────────────────────────────────────────────
static void InitPostProcessing(){
    g_sceneFBO  = LoadRenderTexture(SCREEN_W, SCREEN_H);
    g_brightFBO = LoadRenderTexture(SCREEN_W/2, SCREEN_H/2);
    g_blurA     = LoadRenderTexture(SCREEN_W/2, SCREEN_H/2);
    g_blurB     = LoadRenderTexture(SCREEN_W/2, SCREEN_H/2);
    g_godrayFBO = LoadRenderTexture(SCREEN_W/2, SCREEN_H/2);
    g_shBloomExtract = LoadShaderFromMemory(nullptr, FS_BLOOM_EXTRACT);
    g_shBlur         = LoadShaderFromMemory(nullptr, FS_BLUR);
    g_shGodrays      = LoadShaderFromMemory(nullptr, FS_GODRAYS);
    g_shComposite    = LoadShaderFromMemory(nullptr, FS_COMPOSITE);
    g_shSandstorm    = LoadShaderFromMemory(nullptr, FS_SANDSTORM);
    g_locThreshold = GetShaderLocation(g_shBloomExtract, "threshold");
    g_locBlurDir   = GetShaderLocation(g_shBlur, "direction");
    g_locSunPos    = GetShaderLocation(g_shGodrays, "sunPos");
    g_locBloomTex  = GetShaderLocation(g_shComposite, "bloomTex");
    g_locGodrayTex = GetShaderLocation(g_shComposite, "godrayTex");
    g_locBloomStr  = GetShaderLocation(g_shComposite, "bloomStr");
    g_locGodrayStr = GetShaderLocation(g_shComposite, "godrayStr");
    g_locCompTime     = GetShaderLocation(g_shComposite, "time");
    g_locCompExposure = GetShaderLocation(g_shComposite, "exposure");
    g_locCompRes      = GetShaderLocation(g_shComposite, "resolution");
    g_locStormTime = GetShaderLocation(g_shSandstorm, "time");
    g_locStormRes  = GetShaderLocation(g_shSandstorm, "resolution");
    // ── HD-2D sprite lighting shader ─────────────────────────────────────────
    g_shSpriteLit  = LoadShaderFromMemory(VS_SPRITE_LIT, FS_SPRITE_LIT);
    g_locSpriteSunLR = GetShaderLocation(g_shSpriteLit, "sunLR");
}
static void CleanupPostProcessing(){
    UnloadRenderTexture(g_sceneFBO); UnloadRenderTexture(g_brightFBO);
    UnloadRenderTexture(g_blurA);    UnloadRenderTexture(g_blurB);
    UnloadRenderTexture(g_godrayFBO);
    UnloadShader(g_shBloomExtract); UnloadShader(g_shBlur);
    UnloadShader(g_shGodrays);      UnloadShader(g_shComposite);
    UnloadShader(g_shSandstorm);    UnloadShader(g_shSpriteLit);
}
static void DrawFBOQuad(RenderTexture2D fbo){
    DrawTextureRec(fbo.texture,{0,0,(float)fbo.texture.width,-(float)fbo.texture.height},{0,0},WHITE);
}
static void DrawFBOQuadScaled(RenderTexture2D fbo, int dstW, int dstH){
    Rectangle src={0,0,(float)fbo.texture.width,-(float)fbo.texture.height};
    Rectangle dst={0,0,(float)dstW,(float)dstH};
    DrawTexturePro(fbo.texture,src,dst,{0,0},0,WHITE);
}

// ── Scene IDs ─────────────────────────────────────────────────────────────────
enum Scene { SCENE_OVERWORLD=0, SCENE_TENT1, SCENE_TENT2, SCENE_TENT3, SCENE_TENT4, SCENE_MATCH, SCENE_SHOP };

// ── Structures ────────────────────────────────────────────────────────────────
struct SubCube { Vector3 offset, scale; float rotY; };
struct Rock { int gx,gy; float worldX,worldZ; std::vector<SubCube> cubes; Color baseCol; };
struct Tent { int gx,gy; Color wallCol,roofCol; const char* signText; int doorGX,doorGY; };
struct NPC { int gx,gy; float worldX,worldZ; const char* name; Color shirtCol,pantsCol,hatCol; Texture2D frames[2]; };
struct Particle { float x,y,vx,vy,life,maxLife,size; unsigned char alpha; };
struct Player { int gridX,gridY,targetX,targetY; float visualX,visualZ; Dir dir; bool walking; float walkT; int animFrame; float animTimer; };
// ── Trigger Zone: world-space door triggers with precise alignment ───────────
struct TriggerZone {
    float minX, minZ, maxX, maxZ;   // world-space bounding box (ground plane)
    Scene targetScene;               // where to teleport
    int destGridX, destGridY;        // destination tile
    Dir destDir;                     // facing direction after teleport
    Dir requiredDir;                 // player must face this direction to trigger
};

// ── Globals ───────────────────────────────────────────────────────────────────
static Camera3D  g_cam;
static Camera3D  matchCam;
static Player    g_player;
static Texture2D g_playerSprites[TOTAL_FRAMES];
static Texture2D g_sandTex, g_stormTex, g_torchGlow, g_signTextures[4];
static std::vector<Tent> g_tents;
static std::vector<Rock> g_rocks;
static NPC g_npcs[4];
static float g_time = 0.0f;
static Scene g_scene = SCENE_OVERWORLD;
static int g_savedGridX=0, g_savedGridY=0;
static Dir g_savedDir = DIR_DOWN;
static int g_collision[MAP_H][MAP_W];
static Particle g_particles[MAX_PARTICLES];
static int g_particleCount = 0;
static int g_intCollision[INT_H][INT_W];
static std::vector<TriggerZone> g_triggerZones;   // overworld entrance triggers
static TriggerZone g_exitZone;                     // interior exit trigger
static float g_spriteSunLR = 0.0f;                // cached sun-on-billboard factor
static float g_prevPlayerX = 0.0f;                // previous frame player X (for pan compensation)

// ═══════════════════════════════════════════════════════════════════════════════
// SOVEREIGN HORIZONS — Card Game System
// ═══════════════════════════════════════════════════════════════════════════════

// ── Card Definitions ────────────────────────────────────────────────────────
struct CardDef {
    int id;
    const char* name;
    int cost;
    bool isUnit;
    const char* subtype;
    int atk, def;
    const char* keywords;  // semicolon-separated: "fly;dash;tenacity;overrun"
    const char* effect;    // text description
};

static const CardDef ALL_CARDS[] = {
    // COST 1 UNITS (id 1-34)
    { 1,"Young Thief",1,true,"rogue",4,2,"tenacity",""},
    { 2,"Fast Rat",1,true,"rat",4,1,"dash",""},
    { 3,"Red Rose Warrior",1,true,"plant",0,7,"",""},
    { 4,"Mud Golem",1,true,"golem",2,4,"tenacity",""},
    { 5,"Giant Ant",1,true,"bug",4,3,"",""},
    { 6,"Mad Warrior",1,true,"berserker",5,2,"",""},
    { 7,"Sorcerer Apprentice",1,true,"mage",3,4,"",""},
    { 8,"Ephemeral Angel",1,true,"angel",3,2,"fly",""},
    { 9,"Desert Nomad",1,true,"merchant",2,5,"",""},
    {10,"Hasty Fiend",1,true,"demon",3,2,"dash",""},
    {11,"Flying Ghost",1,true,"spirit",2,3,"fly",""},
    {12,"Wild Beast",1,true,"beast",4,1,"overrun",""},
    {13,"Brave Soldier",1,true,"soldier",4,3,"",""},
    {14,"Undead Servant",1,true,"zombie",4,3,"",""},
    {15,"Quick Rat",1,true,"rat",4,0,"dash","Enter: Opponent discards 1 card."},
    {16,"Desert Rat",1,true,"rat",3,2,"","Enter/Attack: Opponent loses 3 life."},
    {17,"Plague Rat",1,true,"rat",2,3,"","Enter/Death: Put a weak counter on each enemy."},
    {18,"Garden Keeper",1,true,"plant",0,5,"","Harvest: Gain 5 life."},
    {19,"Stone Golem",1,true,"golem",3,3,"","Closure: This unit becomes a defender."},
    {20,"Rock Golem",1,true,"golem",3,4,"","Defend: Put 2 power counters on this unit."},
    {21,"Slaying Mantis",1,true,"bug",5,3,"fly","Attack: You lose 3 life."},
    {22,"Dark Caterpillar",1,true,"bug",3,3,"","Enter: Put 3 weak counters on an enemy."},
    {23,"Angry Pacifist",1,true,"berserker",6,3,"dash","Closure: You lose 3 life."},
    {24,"Furious Avenger",1,true,"berserker",7,5,"overrun","Closure: Discard a card."},
    {25,"Fire Apprentice",1,true,"mage",2,2,"","Enter/Ability: Deal 2 damage."},
    {26,"Dark Apprentice",1,true,"mage",3,3,"","Enter/Ability: Opponent loses 3 life."},
    {27,"Dead Demon of Darkness",1,true,"demon",3,1,"dash","Death: Deal 3 damage and you lose 3 life."},
    {28,"Life Guardian",1,true,"spirit",3,1,"fly","Enter/Attack/Death: Gain 3 life."},
    {29,"Savage Boar",1,true,"beast",4,2,"","Enter: Gain 4 life."},
    {30,"Angry Beast",1,true,"beast",5,4,"dash;overrun","Attack: Discard a card."},
    {31,"Prodigious Recruit",1,true,"soldier",3,3,"","Attack: Put 2 power counters on this unit."},
    {32,"Lucky Recruit",1,true,"soldier",2,4,"","Defend: Put 2 power counters on another ally."},
    {33,"Undead Horde",1,true,"zombie",6,0,"overrun","Cannot defend."},
    {34,"Dead Walkers",1,true,"zombie",6,2,"","Cannot defend."},
    // COST 2 UNITS (id 35-45)
    {35,"Special Tactics Unit",2,true,"rogue",7,4,"tenacity;dash","Attack: Draw 2 cards then discard 2."},
    {36,"Giant Rat",2,true,"rat",6,6,"","Attack: Opponent discards 1 card then loses 3 life."},
    {37,"Persistent Golem",2,true,"golem",7,7,"tenacity","Death: Add this card to hand."},
    {38,"Giant Spider",2,true,"bug",8,8,"","Attack/Defend: Make a 2/2 bug token."},
    {39,"Experimental Failure",2,true,"berserker",12,8,"dash;overrun","Enter: Discard 2 cards then lose 2 life."},
    {40,"Skilled Pyromancer",2,true,"mage",3,9,"","Enter/Ability: Deal 3 damage."},
    {41,"Blessing Spirit",2,true,"spirit",4,4,"fly","Enter: Draw 1 card and gain 4 life."},
    {42,"Savage Beast",2,true,"beast",8,4,"","Enter: Deal 4 damage to an enemy."},
    {43,"Aspiring Sergeant",2,true,"soldier",8,4,"","Enter: Put 4 power counters on another ally."},
    {44,"Honorable Sergeant",2,true,"soldier",7,7,"tenacity","Attack: Put 2 power counters on each other ally."},
    {45,"Rotten Corpse",2,true,"zombie",13,0,"overrun","Cannot defend. Enter: Each player loses 3 life."},
    // COST 3 UNITS (id 46-51)
    {46,"Quick Thief",3,true,"rogue",10,7,"dash","Attack: Draw 3 cards then discard 2."},
    {47,"Silver Golem",3,true,"golem",9,9,"","Death: Gain 3 coins."},
    {48,"Raging Champion",3,true,"berserker",14,7,"dash;overrun","Attack: You lose 3 life."},
    {49,"Master Elementalist",3,true,"mage",6,12,"","Enter/Ability: Deal 5 damage."},
    {50,"Rampaging Rhino",3,true,"beast",12,8,"overrun","Attack: Deal 3 damage to an enemy."},
    {51,"Veteran Commander",3,true,"soldier",9,9,"tenacity","Enter: Put 3 power counters on each other ally."},
    // COST 4+ UNITS (id 52-58)
    {52,"Greedy Thief",4,true,"rogue",11,9,"tenacity","Attack: Gain 2 coins."},
    {53,"Ancient Oak",4,true,"plant",0,15,"","Harvest: Draw 1 card gain 2 coins and gain 3 life."},
    {54,"Grand Sorcerer",4,true,"mage",6,14,"","Enter/Ability: Deal 6 damage."},
    {55,"Tree of Abundance",5,true,"plant",0,16,"","Harvest: Gain 3 coins."},
    {56,"Supreme Archmage",5,true,"mage",7,14,"","Enter/Ability: Deal 7 damage."},
    {57,"Archangel of Mercy",6,true,"angel",12,14,"fly","Enter: Gain 14 life."},
    {58,"Abyssal Overlord",6,true,"demon",13,13,"fly;dash;overrun;tenacity","Enter: Sacrifice a unit then lose 6 life."},
    // SUPPORT CARDS (id 59-75)
    {59,"Arrow",0,false,"",0,0,"","Deal 2 damage."},
    {60,"Luck",0,false,"",0,0,"","Gain 1 coin."},
    {61,"Grow",0,false,"",0,0,"","Put 3 power counters on an ally."},
    {62,"Obsession",0,false,"",0,0,"","Draw 1 card and you lose 4 life."},
    {63,"Filtered Insight",1,false,"",0,0,"","Draw 2 cards then discard 1 card."},
    {64,"Raise Dead",1,false,"",0,0,"","Add a unit from your graveyard to hand."},
    {65,"Fireball",1,false,"",0,0,"","Deal 4 damage."},
    {66,"Firestorm",1,false,"",0,0,"","Deal 3 damage to each unit."},
    {67,"Deep Study",2,false,"",0,0,"","Draw 2 cards."},
    {68,"Assassination",2,false,"",0,0,"","Destroy an enemy."},
    {69,"Rally",2,false,"",0,0,"","Put 4 power counters on each ally."},
    {70,"Necromancy",2,false,"",0,0,"","Deploy a unit with cost 4 or less from your graveyard."},
    {71,"Forced Sacrifice",3,false,"",0,0,"","Your opponent sacrifices 2 units."},
    {72,"Mind Shatter",3,false,"",0,0,"","Your opponent discards 2 cards."},
    {73,"Plague",3,false,"",0,0,"","Put 5 weak counters on each enemy."},
    {74,"Annihilation",5,false,"",0,0,"","Destroy up to 3 enemies."},
    {75,"Rich People's Luck",5,false,"",0,0,"","Gain 7 coins."},
};
static const int NUM_ALL_CARDS = sizeof(ALL_CARDS)/sizeof(ALL_CARDS[0]);

static bool CardHasKeyword(const CardDef& cd, const char* kw) {
    if(!cd.keywords || !cd.keywords[0]) return false;
    return strstr(cd.keywords, kw) != nullptr;
}

// ── Match State ─────────────────────────────────────────────────────────────
static constexpr int MAX_HAND = 10;
static constexpr int MAX_FIELD = 8;
static constexpr int MAX_DECK = 40;
static constexpr int MAX_GRAVE = 60;

struct FieldUnit {
    int cardId;          // references ALL_CARDS by id
    int curAtk, curDef;  // current stats (may be modified by counters)
    int bonusAtk, bonusDef; // For temporary effects
    bool isDefender;
    bool canActivate;    // false on turn deployed (unless Dash)
    bool activated;      // already attacked/used ability this turn
    int powerCounters;   // each adds +1/+1
    int weakCounters;    // each adds -1/-1
    bool alive;
};

struct MatchPlayer {
    int life;
    int coins;
    int hand[MAX_HAND];       // card IDs
    int handSize;
    int deck[MAX_DECK];       // card IDs, deck[deckTop-1] is top
    int deckSize;
    FieldUnit field[MAX_FIELD];
    int fieldSize;
    int grave[MAX_GRAVE];
    int graveSize;
    bool isAI;
};

enum MatchPhase { PHASE_COLLECT, PHASE_DEVELOP, PHASE_ACTIVATE, PHASE_END, PHASE_GAME_OVER };
enum MatchAction { ACT_NONE, ACT_SELECT_HAND, ACT_SELECT_FIELD, ACT_SELECT_TARGET, ACT_CONFIRM };

struct GameMatch {
    MatchPlayer players[2];  // 0=human, 1=AI
    int turn;                // whose turn (0 or 1)
    MatchPhase phase;
    int turnNumber;
    bool active;
    bool playerWon;
    int selectedHandIdx;     // for human input
    int selectedFieldIdx;
    int targetFieldIdx;
    MatchAction pendingAction;
    int challengedNPC;       // which NPC started this match
    float messageTimer;
    char message[128];
    // For support card targeting
    int pendingSupportCard;  // card id of support being resolved, -1 if none
    bool needsTarget;
};

static GameMatch g_match;

// ── Player Collection & Shop ────────────────────────────────────────────────
static constexpr int MAX_COLLECTION = 400;
static int g_collection[MAX_COLLECTION]; // card IDs owned
static int g_collectionSize = 0;
static int g_playerDeck[MAX_DECK];       // current deck (card IDs)
static int g_playerDeckSize = 0;
static int g_playerCoins = 0;            // persistent coins for shop
static bool g_hasStarterDeck = false;
static int g_shopScroll = 0;

// ── Menu System ─────────────────────────────────────────────────────────────
static bool g_menuOpen = false;
static bool g_npcDialogOpen = false;
static int  g_targetNPC = -1;
static int  g_dialogSelection = 0;
enum MenuTab { TAB_COLLECTION=0, TAB_DECKS, TAB_COUNT };
static MenuTab g_menuTab = TAB_COLLECTION;
static int g_collScroll = 0;        // collection browser scroll
static int g_selectedCardId = -1;   // selected card in collection view
static int g_deckScroll = 0;        // deckbuilder deck list scroll

// Card copies tracking
struct CardCopy { int cardId; int count; };
static CardCopy g_cardCopies[80];   // aggregated collection
static int g_numCardCopies = 0;

static void RebuildCardCopies() {
    g_numCardCopies = 0;
    for(int i = 0; i < g_collectionSize; i++) {
        int cid = g_collection[i];
        bool found = false;
        for(int j = 0; j < g_numCardCopies; j++) {
            if(g_cardCopies[j].cardId == cid) { g_cardCopies[j].count++; found=true; break; }
        }
        if(!found && g_numCardCopies < 80) {
            g_cardCopies[g_numCardCopies++] = {cid, 1};
        }
    }
}

// ── Starter Deck ────────────────────────────────────────────────────────────
static void GiveStarterDeck() {
    // 30 cards: mix of cost 0-2
    const int starterIds[] = {
        1,2,3,4,5,6,7,8,9,10,   // 10 cheap units
        11,12,13,14,15,16,17,18, // 8 more cheap units
        59,59,60,61,62,65,65,    // 7 support cards
        25,26,29,31,13           // 5 more units
    };
    g_playerDeckSize = 30;
    for(int i=0;i<30;i++) g_playerDeck[i] = starterIds[i];
    // Also add to collection
    for(int i=0;i<30;i++) {
        if(g_collectionSize < MAX_COLLECTION)
            g_collection[g_collectionSize++] = starterIds[i];
    }
    g_hasStarterDeck = true;
}

// ── Deck Building for NPC ───────────────────────────────────────────────────
static void BuildNPCDeck(int npcIdx, int* deck, int& deckSize) {
    deckSize = 30;
    switch(npcIdx) {
    case 0: { // Fatima: Rat/Beast aggro
        const int ids[] = {2,2,15,15,16,16,17,36,36,12,12,30,30,29,29,42,5,5,6,6,59,59,65,65,61,61,62,63,13,14};
        for(int i=0;i<30;i++) deck[i]=ids[i];
    } break;
    case 1: { // Sage Karim: Mage/Spirit control
        const int ids[] = {7,7,25,25,26,26,40,40,49,56,8,8,11,11,28,28,41,41,65,65,66,66,68,68,67,67,63,63,59,61};
        for(int i=0;i<30;i++) deck[i]=ids[i];
    } break;
    case 2: { // Merchant Yara: Plant/Merchant value
        const int ids[] = {3,3,18,18,53,55,9,9,60,60,60,75,67,67,69,69,61,61,29,29,4,4,37,47,59,59,63,63,64,64};
        for(int i=0;i<30;i++) deck[i]=ids[i];
    } break;
    case 3: { // Chief Omari: Demon/Zombie aggro
        const int ids[] = {10,10,27,27,33,33,34,34,45,58,23,23,39,48,6,6,14,14,30,30,59,59,65,65,62,62,68,71,72,73};
        for(int i=0;i<30;i++) deck[i]=ids[i];
    } break;
    }
}

// ── Shuffle Array ───────────────────────────────────────────────────────────
static void ShuffleDeck(int* arr, int n) {
    for(int i=n-1;i>0;i--) {
        int j = rand()%(i+1);
        int tmp=arr[i]; arr[i]=arr[j]; arr[j]=tmp;
    }
}

// ── Find CardDef by ID ──────────────────────────────────────────────────────
static const CardDef& GetCard(int id) {
    for(int i=0;i<NUM_ALL_CARDS;i++)
        if(ALL_CARDS[i].id==id) return ALL_CARDS[i];
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

    for(int p=0;p<2;p++) {
        MatchPlayer& mp = g_match.players[p];
        mp.life = 20;
        mp.coins = 0;
        mp.handSize = 0;
        mp.fieldSize = 0;
        mp.graveSize = 0;
        mp.isAI = (p==1);

        if(p==0) {
            // Player deck
            mp.deckSize = g_playerDeckSize;
            for(int i=0;i<mp.deckSize;i++) mp.deck[i] = g_playerDeck[i];
        } else {
            // NPC deck
            BuildNPCDeck(npcIdx, mp.deck, mp.deckSize);
        }
        ShuffleDeck(mp.deck, mp.deckSize);

        // Draw starting hand of 5
        for(int i=0;i<5 && mp.deckSize>0;i++) {
            mp.hand[mp.handSize++] = mp.deck[--mp.deckSize];
        }
    }
}

// ── Draw a card from deck to hand ───────────────────────────────────────────
static bool MatchDrawCard(MatchPlayer& mp) {
    if(mp.deckSize <= 0) return false;
    if(mp.handSize >= MAX_HAND) return false; // hand full, card lost
    mp.hand[mp.handSize++] = mp.deck[--mp.deckSize];
    return true;
}

// ── Discard from hand ───────────────────────────────────────────────────────
static void MatchDiscard(MatchPlayer& mp, int handIdx) {
    if(handIdx < 0 || handIdx >= mp.handSize) return;
    if(mp.graveSize < MAX_GRAVE) mp.grave[mp.graveSize++] = mp.hand[handIdx];
    for(int i=handIdx;i<mp.handSize-1;i++) mp.hand[i]=mp.hand[i+1];
    mp.handSize--;
}

// ── Deploy a unit from hand to field ────────────────────────────────────────
static bool MatchDeployUnit(GameMatch& m, int playerIdx, int handIdx) {
    MatchPlayer& mp = m.players[playerIdx];
    if(handIdx < 0 || handIdx >= mp.handSize) return false;
    int cardId = mp.hand[handIdx];
    const CardDef& cd = GetCard(cardId);
    if(!cd.isUnit) return false;
    if(mp.coins < cd.cost) return false;
    if(mp.fieldSize >= MAX_FIELD) return false;

    mp.coins -= cd.cost;
    FieldUnit& fu = mp.field[mp.fieldSize++];
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

    // Remove from hand
    for(int i=handIdx;i<mp.handSize-1;i++) mp.hand[i]=mp.hand[i+1];
    mp.handSize--;

    // Simple Enter effects
    MatchPlayer& opp = m.players[1-playerIdx];
    if(strstr(cd.effect, "Enter") && strstr(cd.effect, "Gain") && strstr(cd.effect, "life")) {
        // Extract number
        const char* p = strstr(cd.effect, "Gain");
        if(p) { int val = atoi(p+5); mp.life += val; }
    }
    if(strstr(cd.effect, "Enter") && strstr(cd.effect, "Deal") && strstr(cd.effect, "damage")) {
        const char* p = strstr(cd.effect, "Deal");
        if(p) {
            int val = atoi(p+5);
            // Deal damage to weakest enemy unit if possible
            if(opp.fieldSize > 0) {
                int weakest=0;
                for(int i=1;i<opp.fieldSize;i++)
                    if(opp.field[i].curDef + opp.field[i].powerCounters - opp.field[i].weakCounters <
                       opp.field[weakest].curDef + opp.field[weakest].powerCounters - opp.field[weakest].weakCounters)
                        weakest=i;
                opp.field[weakest].curDef -= val;
                if(opp.field[weakest].curDef + opp.field[weakest].powerCounters - opp.field[weakest].weakCounters <= 0)
                    opp.field[weakest].alive = false;
            } else {
                opp.life -= val;
            }
        }
    }
    if(strstr(cd.effect, "Enter") && strstr(cd.effect, "Opponent loses") && strstr(cd.effect, "life")) {
        const char* p = strstr(cd.effect, "loses");
        if(p) { int val = atoi(p+6); opp.life -= val; }
    }
    if(strstr(cd.effect, "Enter") && strstr(cd.effect, "Opponent discards")) {
        if(opp.handSize > 0) MatchDiscard(opp, rand()%opp.handSize);
    }
    if(strstr(cd.effect, "Enter") && strstr(cd.effect, "Draw")) {
        const char* p = strstr(cd.effect, "Draw");
        if(p) { int val = atoi(p+5); for(int i=0;i<val;i++) MatchDrawCard(mp); }
    }
    if(strstr(cd.effect, "Enter") && strstr(cd.effect, "power counters on")) {
        const char* p = strstr(cd.effect, "Put");
        if(p) {
            int val = atoi(p+4);
            if(strstr(cd.effect, "each other ally")) {
                for(int i=0;i<mp.fieldSize;i++)
                    if(i != mp.fieldSize-1) mp.field[i].powerCounters += val;
            } else if(strstr(cd.effect, "another ally")) {
                // Buff strongest other ally
                if(mp.fieldSize > 1) {
                    int best=0; if(best==mp.fieldSize-1) best=1;
                    for(int i=0;i<mp.fieldSize;i++) {
                        if(i==mp.fieldSize-1) continue;
                        if(mp.field[i].curAtk+mp.field[i].powerCounters > mp.field[best].curAtk+mp.field[best].powerCounters) best=i;
                    }
                    mp.field[best].powerCounters += val;
                }
            }
        }
    }
    if(strstr(cd.effect, "Enter") && strstr(cd.effect, "weak counter")) {
        const char* p = strstr(cd.effect, "Put");
        if(p) {
            int val = atoi(p+4);
            if(strstr(cd.effect, "each enemy")) {
                for(int i=0;i<opp.fieldSize;i++) opp.field[i].weakCounters += val;
            } else if(strstr(cd.effect, "an enemy") && opp.fieldSize > 0) {
                // Weakest enemy
                int weakest=0;
                for(int i=1;i<opp.fieldSize;i++)
                    if(opp.field[i].curDef < opp.field[weakest].curDef) weakest=i;
                opp.field[weakest].weakCounters += val;
            }
        }
    }
    if(strstr(cd.effect, "Enter") && strstr(cd.effect, "Each player loses")) {
        const char* p = strstr(cd.effect, "loses");
        if(p) { int val = atoi(p+6); mp.life -= val; opp.life -= val; }
    }
    if(strstr(cd.effect, "Enter") && strstr(cd.effect, "Discard")) {
        const char* p = strstr(cd.effect, "Discard");
        if(p) { int val = atoi(p+8); for(int i=0;i<val && mp.handSize>0;i++) MatchDiscard(mp, rand()%mp.handSize); }
    }
    if(strstr(cd.effect, "Enter") && strstr(cd.effect, "lose") && strstr(cd.effect, "life") && !strstr(cd.effect, "Opponent")) {
        // "you lose X life" from enter
        const char* p = strstr(cd.effect, "lose");
        if(p && !strstr(cd.effect, "Each player")) { int val = atoi(p+5); mp.life -= val; }
    }
    if(strstr(cd.effect, "Enter") && strstr(cd.effect, "Sacrifice a unit")) {
        // Sacrifice weakest own unit (not the one just deployed)
        if(mp.fieldSize > 1) {
            int weakest=0; if(weakest==mp.fieldSize-1) weakest=1;
            for(int i=0;i<mp.fieldSize-1;i++)
                if(mp.field[i].curDef < mp.field[weakest].curDef) weakest=i;
            mp.field[weakest].alive = false;
        }
        mp.life -= 6;
    }
    if(strstr(cd.effect, "Enter") && strstr(cd.effect, "gain") && strstr(cd.effect, "coin")) {
        const char* p = strstr(cd.effect, "gain");
        if(p) { int val = atoi(p+5); mp.coins += val; }
    }

    return true;
}

// ── Remove dead units from field ────────────────────────────────────────────
static void CleanupField(MatchPlayer& mp) {
    int w=0;
    for(int i=0;i<mp.fieldSize;i++) {
        int effDef = mp.field[i].curDef + mp.field[i].powerCounters - mp.field[i].weakCounters;
        if(!mp.field[i].alive || effDef <= 0) {
            if(mp.graveSize < MAX_GRAVE) mp.grave[mp.graveSize++] = mp.field[i].cardId;
            continue;
        }
        if(w!=i) mp.field[w] = mp.field[i];
        w++;
    }
    mp.fieldSize = w;
}

// ── Play Support Card ───────────────────────────────────────────────────────
static void PlaySupportCard(GameMatch& m, int playerIdx, int handIdx) {
    MatchPlayer& mp = m.players[playerIdx];
    MatchPlayer& opp = m.players[1-playerIdx];
    int cardId = mp.hand[handIdx];
    const CardDef& cd = GetCard(cardId);
    if(mp.coins < cd.cost) return;
    mp.coins -= cd.cost;

    // Execute effect
    if(cardId==59) { // Arrow: Deal 2 damage
        if(opp.fieldSize>0) { int t=rand()%opp.fieldSize; opp.field[t].curDef-=2; }
        else opp.life -= 2;
    }
    else if(cardId==60) { mp.coins += 1; } // Luck
    else if(cardId==61) { // Grow
        if(mp.fieldSize>0) mp.field[rand()%mp.fieldSize].powerCounters += 3;
    }
    else if(cardId==62) { MatchDrawCard(mp); mp.life -= 4; } // Obsession
    else if(cardId==63) { MatchDrawCard(mp); MatchDrawCard(mp); if(mp.handSize>0) MatchDiscard(mp,rand()%mp.handSize); } // Filtered Insight
    else if(cardId==64) { // Raise Dead
        if(mp.graveSize>0) {
            for(int i=mp.graveSize-1;i>=0;i--) {
                if(GetCard(mp.grave[i]).isUnit && mp.handSize<MAX_HAND) {
                    mp.hand[mp.handSize++] = mp.grave[i];
                    for(int j=i;j<mp.graveSize-1;j++) mp.grave[j]=mp.grave[j+1];
                    mp.graveSize--;
                    break;
                }
            }
        }
    }
    else if(cardId==65) { // Fireball: Deal 4 damage
        if(opp.fieldSize>0) { int t=rand()%opp.fieldSize; opp.field[t].curDef-=4; }
        else opp.life -= 4;
    }
    else if(cardId==66) { // Firestorm: 3 to each unit
        for(int i=0;i<mp.fieldSize;i++) mp.field[i].curDef -= 3;
        for(int i=0;i<opp.fieldSize;i++) opp.field[i].curDef -= 3;
    }
    else if(cardId==67) { MatchDrawCard(mp); MatchDrawCard(mp); } // Deep Study
    else if(cardId==68) { // Assassination
        if(opp.fieldSize>0) opp.field[rand()%opp.fieldSize].alive = false;
    }
    else if(cardId==69) { // Rally
        for(int i=0;i<mp.fieldSize;i++) mp.field[i].powerCounters += 4;
    }
    else if(cardId==70) { // Necromancy
        for(int i=mp.graveSize-1;i>=0;i--) {
            const CardDef& gc = GetCard(mp.grave[i]);
            if(gc.isUnit && gc.cost<=4 && mp.fieldSize<MAX_FIELD) {
                FieldUnit& fu = mp.field[mp.fieldSize++];
                fu.cardId = mp.grave[i]; fu.curAtk=gc.atk; fu.curDef=gc.def;
                fu.bonusAtk=0; fu.bonusDef=0;
                fu.isDefender=false; fu.canActivate=CardHasKeyword(gc,"dash");
                fu.activated=false; fu.powerCounters=0; fu.weakCounters=0; fu.alive=true;
                for(int j=i;j<mp.graveSize-1;j++) mp.grave[j]=mp.grave[j+1];
                mp.graveSize--;
                break;
            }
        }
    }
    else if(cardId==71) { // Forced Sacrifice
        for(int k=0;k<2 && opp.fieldSize>0;k++) {
            int weakest=0;
            for(int i=1;i<opp.fieldSize;i++)
                if(opp.field[i].curAtk+opp.field[i].powerCounters < opp.field[weakest].curAtk+opp.field[weakest].powerCounters) weakest=i;
            opp.field[weakest].alive = false;
            CleanupField(opp);
        }
    }
    else if(cardId==72) { // Mind Shatter
        for(int k=0;k<2 && opp.handSize>0;k++) MatchDiscard(opp, rand()%opp.handSize);
    }
    else if(cardId==73) { // Plague
        for(int i=0;i<opp.fieldSize;i++) opp.field[i].weakCounters += 5;
    }
    else if(cardId==74) { // Annihilation
        for(int k=0;k<3 && opp.fieldSize>0;k++) {
            opp.field[rand()%opp.fieldSize].alive = false;
            CleanupField(opp);
        }
    }
    else if(cardId==75) { mp.coins += 7; } // Rich People's Luck

    // Remove card from hand to graveyard
    if(mp.graveSize < MAX_GRAVE) mp.grave[mp.graveSize++] = cardId;
    for(int i=handIdx;i<mp.handSize-1;i++) mp.hand[i]=mp.hand[i+1];
    mp.handSize--;

    CleanupField(mp);
    CleanupField(opp);
}

// ── Combat Resolution ───────────────────────────────────────────────────────
static void ResolveCombat(GameMatch& m, int atkPlayer, int atkIdx, int defPlayer, int defIdx) {
    MatchPlayer& ap = m.players[atkPlayer];
    MatchPlayer& dp = m.players[defPlayer];
    FieldUnit& attacker = ap.field[atkIdx];
    const CardDef& acd = GetCard(attacker.cardId);
    int aAtk = attacker.curAtk + attacker.powerCounters - attacker.weakCounters + attacker.bonusAtk;
    if(aAtk < 0) aAtk = 0;

    if(defIdx < 0) {
        // Direct attack on player
        dp.life -= aAtk;
        attacker.activated = true;
        // Attack trigger effects
        if(strstr(acd.effect,"Attack") && strstr(acd.effect,"Gain") && strstr(acd.effect,"coin")) {
            const char* p = strstr(acd.effect,"Gain"); if(p) ap.coins += atoi(p+5);
        }
        if(strstr(acd.effect,"Attack") && strstr(acd.effect,"Gain") && strstr(acd.effect,"life")) {
            const char* p = strstr(acd.effect,"Gain"); if(p) ap.life += atoi(p+5);
        }
        return;
    }

    FieldUnit& defender = dp.field[defIdx];
    const CardDef& dcd = GetCard(defender.cardId);
    int dAtk = defender.curAtk + defender.powerCounters - defender.weakCounters + defender.bonusAtk;
    int dDef = defender.curDef + defender.powerCounters - defender.weakCounters + defender.bonusDef;
    if(dAtk < 0) dAtk = 0;
    if(dDef < 0) dDef = 0;
    int aDef = attacker.curDef + attacker.powerCounters - attacker.weakCounters + attacker.bonusDef;
    if(aDef < 0) aDef = 0;

    attacker.activated = true;

    bool atkTenacity = CardHasKeyword(acd, "tenacity");
    bool defTenacity = CardHasKeyword(dcd, "tenacity");
    bool atkOverrun = CardHasKeyword(acd, "overrun");

    // Compare ATK vs DEF
    if(aAtk > dDef) {
        defender.alive = false;
        if(atkOverrun) dp.life -= (aAtk - dDef);
    } else if(aAtk == dDef) {
        defender.alive = false;
        if(!atkTenacity) attacker.alive = false;
    } else {
        // Defender survives, reduce DEF temporarily
        defender.curDef -= aAtk;
    }

    // Defender retaliates
    if(dAtk > aDef) {
        attacker.alive = false;
    } else if(dAtk == aDef) {
        if(!defTenacity) attacker.alive = false;
        // defender already handled above
    }

    // Attack trigger effects
    if(strstr(acd.effect,"Attack") && strstr(acd.effect,"You lose") && strstr(acd.effect,"life")) {
        const char* p = strstr(acd.effect,"lose"); if(p) ap.life -= atoi(p+5);
    }
    if(strstr(acd.effect,"Attack") && strstr(acd.effect,"Opponent loses") && strstr(acd.effect,"life")) {
        const char* p = strstr(acd.effect,"loses"); if(p) dp.life -= atoi(p+6);
    }
    if(strstr(acd.effect,"Attack") && strstr(acd.effect,"Gain") && strstr(acd.effect,"coin")) {
        const char* p = strstr(acd.effect,"Gain"); if(p) ap.coins += atoi(p+5);
    }
    if(strstr(acd.effect,"Attack") && strstr(acd.effect,"Draw")) {
        const char* p = strstr(acd.effect,"Draw"); if(p) { int val=atoi(p+5); for(int i=0;i<val;i++) MatchDrawCard(ap); }
    }
    if(strstr(acd.effect,"Attack") && strstr(acd.effect,"Discard")) {
        if(strstr(acd.effect,"Opponent discards") && dp.handSize>0) MatchDiscard(dp, rand()%dp.handSize);
        else if(ap.handSize>0) MatchDiscard(ap, rand()%ap.handSize);
    }
    if(strstr(acd.effect,"Attack") && strstr(acd.effect,"power counter")) {
        const char* p = strstr(acd.effect,"Put"); if(p) {
            int val=atoi(p+4);
            if(strstr(acd.effect,"each other ally")) { for(int i=0;i<ap.fieldSize;i++) if(i!=atkIdx) ap.field[i].powerCounters+=val; }
            else if(strstr(acd.effect,"on this unit")) { attacker.powerCounters+=val; }
        }
    }

    CleanupField(ap);
    CleanupField(dp);
}

// ── AI Logic ────────────────────────────────────────────────────────────────
static void AITakeTurn(GameMatch& m) {
    MatchPlayer& ai = m.players[1];
    MatchPlayer& human = m.players[0];

    // Collect phase
    if(!MatchDrawCard(ai)) { m.phase = PHASE_GAME_OVER; m.playerWon = true; m.active = false; return; }
    ai.coins += 2;

    // Development phase: play most expensive affordable cards
    bool played = true;
    while(played) {
        played = false;
        int bestIdx = -1, bestCost = -1;
        for(int i=0;i<ai.handSize;i++) {
            const CardDef& cd = GetCard(ai.hand[i]);
            if(cd.isUnit && cd.cost <= ai.coins && ai.fieldSize < MAX_FIELD && cd.cost > bestCost) {
                bestCost = cd.cost; bestIdx = i;
            }
        }
        if(bestIdx >= 0) {
            MatchDeployUnit(m, 1, bestIdx);
            CleanupField(ai); CleanupField(human);
            played = true;
        }
    }
    // Play affordable support cards
    for(int i=ai.handSize-1;i>=0;i--) {
        const CardDef& cd = GetCard(ai.hand[i]);
        if(!cd.isUnit && cd.cost <= ai.coins) {
            PlaySupportCard(m, 1, i);
        }
    }

    // Activation phase: attack with all available units
    // Find defenders first
    bool hasDefenders = false;
    for(int i=0;i<human.fieldSize;i++) if(human.field[i].isDefender) { hasDefenders = true; break; }

    for(int a=0;a<ai.fieldSize;a++) {
        if(!ai.field[a].canActivate || ai.field[a].activated) continue;
        const CardDef& acd = GetCard(ai.field[a].cardId);
        if(strstr(acd.effect,"Cannot defend.") && false) continue; // this is about defending, not attacking
        bool hasFly = CardHasKeyword(acd, "fly");

        // Recalculate defenders
        hasDefenders = false;
        for(int i=0;i<human.fieldSize;i++) if(human.field[i].isDefender) { hasDefenders = true; break; }

        if(hasDefenders && !hasFly) {
            // Must attack a defender
            int weakestDef = -1, weakestVal = 99999;
            for(int d=0;d<human.fieldSize;d++) {
                if(!human.field[d].isDefender) continue;
                int val = human.field[d].curDef + human.field[d].powerCounters - human.field[d].weakCounters;
                if(val < weakestVal) { weakestVal=val; weakestDef=d; }
            }
            if(weakestDef >= 0) ResolveCombat(m, 1, a, 0, weakestDef);
        } else {
            // Attack weakest unit or player
            if(human.fieldSize > 0 && !hasFly) {
                int weakest=0;
                for(int d=1;d<human.fieldSize;d++)
                    if(human.field[d].curDef + human.field[d].powerCounters - human.field[d].weakCounters <
                       human.field[weakest].curDef + human.field[weakest].powerCounters - human.field[weakest].weakCounters)
                        weakest=d;
                ResolveCombat(m, 1, a, 0, weakest);
            } else {
                ResolveCombat(m, 1, a, 0, -1);
            }
        }
        CleanupField(ai); CleanupField(human);
        if(human.life <= 0 || ai.life <= 0) break;
    }

    // AI assigns defenders: highest DEF units
    for(int i=0;i<ai.fieldSize;i++) ai.field[i].isDefender = false;
    if(ai.fieldSize > 0) {
        // Make the highest DEF units defenders (up to half)
        int numDef = (ai.fieldSize+1)/2;
        // Simple: just mark units with highest DEF
        for(int k=0;k<numDef;k++) {
            int bestIdx=-1, bestDef=-1;
            for(int i=0;i<ai.fieldSize;i++) {
                if(ai.field[i].isDefender) continue;
                const CardDef& cd = GetCard(ai.field[i].cardId);
                if(strstr(cd.effect,"Cannot defend.")) continue;
                int d = ai.field[i].curDef + ai.field[i].powerCounters - ai.field[i].weakCounters;
                if(d > bestDef) { bestDef=d; bestIdx=i; }
            }
            if(bestIdx>=0) ai.field[bestIdx].isDefender = true;
        }
    }

    // End turn: prepare for player
    for(int i=0;i<ai.fieldSize;i++) {
        ai.field[i].canActivate = true;
        ai.field[i].activated = false;
    }

    // Check win/loss
    if(human.life <= 0) { m.phase = PHASE_GAME_OVER; m.playerWon = false; m.active = false; return; }
    if(ai.life <= 0) { m.phase = PHASE_GAME_OVER; m.playerWon = true; m.active = false; return; }
}

// ── Update Match (human turn logic) ─────────────────────────────────────────
static void UpdateMatch(float dt) {
    GameMatch& m = g_match;
    if(!m.active && m.phase == PHASE_GAME_OVER) {
        if(m.messageTimer > 0) m.messageTimer -= dt;
        if(IsKeyPressed(KEY_ENTER) || IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if(m.playerWon) g_playerCoins += 10;
            g_scene = SCENE_OVERWORLD;
        }
        return;
    }
    if(m.messageTimer > 0) m.messageTimer -= dt;

    MatchPlayer& human = m.players[0];
    MatchPlayer& ai = m.players[1];

    if(m.turn == 0) { // Human turn
        if(m.phase == PHASE_COLLECT) {
            if(!MatchDrawCard(human)) { m.phase = PHASE_GAME_OVER; m.playerWon = false; m.active = false; return; }
            human.coins += 2;
            m.phase = PHASE_DEVELOP;
            snprintf(m.message, 128, "Your turn - Development Phase (play cards)");
            m.messageTimer = 1.5f;
        }

        if(m.phase == PHASE_DEVELOP) {
            Vector2 mouse = GetMousePosition();

            // Click on hand cards to play them
            for(int i=0;i<human.handSize;i++) {
                float cx = 60 + i * 95;
                float cy = SCREEN_H - 120;
                Rectangle cardRect = {cx, cy, 85, 110};
                if(CheckCollisionPointRec(mouse, cardRect) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    const CardDef& cd = GetCard(human.hand[i]);
                    if(cd.isUnit) {
                        if(MatchDeployUnit(m, 0, i)) {
                            CleanupField(human); CleanupField(ai);
                            snprintf(m.message, 128, "Deployed %s", cd.name);
                            m.messageTimer = 1.0f;
                        } else {
                            snprintf(m.message, 128, "Can't deploy: need %d coins (have %d)", cd.cost, human.coins);
                            m.messageTimer = 1.0f;
                        }
                    } else {
                        if(human.coins >= cd.cost) {
                            PlaySupportCard(m, 0, i);
                            snprintf(m.message, 128, "Played %s", cd.name);
                            m.messageTimer = 1.0f;
                        } else {
                            snprintf(m.message, 128, "Need %d coins (have %d)", cd.cost, human.coins);
                            m.messageTimer = 1.0f;
                        }
                    }
                    break;
                }
            }

            // Press Space/Enter to advance to Activate phase
            if(IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) {
                m.phase = PHASE_ACTIVATE;
                m.selectedFieldIdx = -1;
                snprintf(m.message, 128, "Activation Phase - click your units to attack");
                m.messageTimer = 1.5f;
            }
        }

        if(m.phase == PHASE_ACTIVATE) {
            Vector2 mouse = GetMousePosition();

            // Select own unit to attack with
            if(m.selectedFieldIdx < 0) {
                for(int i=0;i<human.fieldSize;i++) {
                    float cx = 60 + i * 110;
                    float cy = SCREEN_H/2 - 30;
                    Rectangle unitRect = {cx, cy, 100, 65};
                    if(CheckCollisionPointRec(mouse, unitRect) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        if(human.field[i].canActivate && !human.field[i].activated) {
                            m.selectedFieldIdx = i;
                            snprintf(m.message, 128, "Select target (opponent unit or click 'Attack Player')");
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
                for(int i=0;i<ai.fieldSize;i++) if(ai.field[i].isDefender) { hasDefenders = true; break; }
                bool atkHasFly = CardHasKeyword(GetCard(human.field[m.selectedFieldIdx].cardId), "fly");

                for(int i=0;i<ai.fieldSize;i++) {
                    float cx = 60 + i * 110;
                    float cy = 70;
                    Rectangle unitRect = {cx, cy, 100, 65};
                    if(CheckCollisionPointRec(mouse, unitRect) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        if(hasDefenders && !ai.field[i].isDefender && !atkHasFly) {
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
                Rectangle atkPlayerBtn = {(float)SCREEN_W-180, 20, 160, 30};
                if(CheckCollisionPointRec(mouse, atkPlayerBtn) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    if(hasDefenders && !atkHasFly) {
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
                if(IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
                    m.selectedFieldIdx = -1;
                }
            }

            // Assign defenders: click 'D' to toggle defender on selected unit
            if(IsKeyPressed(KEY_D)) {
                for(int i=0;i<human.fieldSize;i++) {
                    float cx = 60 + i * 110;
                    float cy = SCREEN_H/2 - 30;
                    Rectangle unitRect = {cx, cy, 100, 65};
                    if(CheckCollisionPointRec(mouse, unitRect)) {
                        const CardDef& cd = GetCard(human.field[i].cardId);
                        if(!strstr(cd.effect,"Cannot defend."))
                            human.field[i].isDefender = !human.field[i].isDefender;
                        break;
                    }
                }
            }

            // Press Space/Enter to end turn
            if(IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) {
                m.phase = PHASE_END;
            }
        }

        if(m.phase == PHASE_END) {
            // Reset activation for next turn
            for(int i=0;i<human.fieldSize;i++) {
                human.field[i].canActivate = true;
                human.field[i].activated = false;
            }
            // Check win/loss
            if(ai.life <= 0) { m.phase = PHASE_GAME_OVER; m.playerWon = true; m.active = false;
                snprintf(m.message, 128, "You win! Press Enter to continue."); m.messageTimer = 99.0f; return; }
            if(human.life <= 0) { m.phase = PHASE_GAME_OVER; m.playerWon = false; m.active = false;
                snprintf(m.message, 128, "You lose! Press Enter to continue."); m.messageTimer = 99.0f; return; }

            // AI turn
            m.turn = 1;
            m.turnNumber++;
            AITakeTurn(m);

            // Check again
            if(ai.life <= 0) { m.phase = PHASE_GAME_OVER; m.playerWon = true; m.active = false;
                snprintf(m.message, 128, "You win! Press Enter to continue."); m.messageTimer = 99.0f; return; }
            if(human.life <= 0) { m.phase = PHASE_GAME_OVER; m.playerWon = false; m.active = false;
                snprintf(m.message, 128, "You lose! Press Enter to continue."); m.messageTimer = 99.0f; return; }

            // Back to player
            m.turn = 0;
            m.phase = PHASE_COLLECT;
        }
    }
}

// ── Draw Match Scene ────────────────────────────────────────────────────────
static void DrawMatchScene() {
    GameMatch& m = g_match;
    MatchPlayer& human = m.players[0];
    MatchPlayer& ai = m.players[1];

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
    for(int i=0;i<ai.handSize;i++) {
        float totalWidth = ai.handSize * 1.2f;
        float startX = -totalWidth / 2.0f + 0.6f;
        float cx = startX + i * 1.2f;
        DrawCube((Vector3){cx, 0, oppHandZ}, 1.0f, 0.1f, 0.7f, {80,60,40,255});
        DrawCubeWires((Vector3){cx, 0, oppHandZ}, 1.0f, 0.1f, 0.7f, {120,100,60,255});
    }

    // ── Opponent field (units)
    for(int i=0;i<ai.fieldSize;i++) {
        float totalWidth = ai.fieldSize * (cardWidth + 0.2f);
        float startX = -totalWidth / 2.0f + cardWidth / 2.0f;
        float cx = startX + i * (cardWidth + 0.2f);
        Color bg = ai.field[i].isDefender ? Color{60,60,120,255} : Color{70,50,35,255};
        DrawCube((Vector3){cx, 0, oppFieldZ}, cardWidth, 0.2f, cardHeight, bg);
        DrawCubeWires((Vector3){cx, 0, oppFieldZ}, cardWidth, 0.2f, cardHeight, {180,150,80,255});
    }

    // Player cards are positioned slightly closer to the camera.
    float playerFieldZ = -4.0f;
    float playerHandZ = -9.0f;

    // ── Player field (units)
    for(int i=0;i<human.fieldSize;i++) {
        float totalWidth = human.fieldSize * (cardWidth + 0.2f);
        float startX = -totalWidth / 2.0f + cardWidth / 2.0f;
        float cx = startX + i * (cardWidth + 0.2f);
        Color bg = {50,40,30,255};
        if(human.field[i].isDefender) bg = {40,40,90,255};
        if(m.selectedFieldIdx == i) bg = {100,80,40,255};
        if(!human.field[i].canActivate) bg = {40,35,25,255};
        DrawCube((Vector3){cx, 0, playerFieldZ}, cardWidth, 0.2f, cardHeight, bg);
        Color border = (human.field[i].canActivate && !human.field[i].activated) ?
            Color{255,220,80,255} : Color{120,100,60,255};
        DrawCubeWires((Vector3){cx, 0, playerFieldZ}, cardWidth, 0.2f, cardHeight, border);
    }

    // ── Player hand (full cards)
    float handCardWidth = 2.2f;
    float handCardHeight = 3.0f;
    for(int i=0;i<human.handSize;i++) {
        const CardDef& cd = GetCard(human.hand[i]);
        float totalWidth = human.handSize * (handCardWidth + 0.2f);
        float startX = -totalWidth / 2.0f + handCardWidth / 2.0f;
        float cx = startX + i * (handCardWidth + 0.2f);
        Color bg = cd.isUnit ? Color{50,60,50,255} : Color{60,50,60,255};
        if(cd.cost > human.coins) bg = {40,30,30,255}; // dim if can't afford
        
        // NOTE: Drawing text and detailed card art in a 3D perspective requires a more 
        // complex solution (e.g., rendering cards to a texture and drawing that texture).
        // For this 3D transformation, we are only drawing colored cubes as placeholders.
        DrawCube((Vector3){cx, 0, playerHandZ}, handCardWidth, 0.1f, handCardHeight, bg);
        DrawCubeWires((Vector3){cx, 0, playerHandZ}, handCardWidth, 0.1f, handCardHeight, {160,140,80,255});
    }
}

// ── Shop System ─────────────────────────────────────────────────────────────
static void UpdateShop(float dt) {
    (void)dt;
    Vector2 mouse = GetMousePosition();

    // Buy starter deck button
    if(!g_hasStarterDeck) {
        Rectangle btn = {SCREEN_W/2-100, 200, 200, 40};
        if(CheckCollisionPointRec(mouse, btn) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            GiveStarterDeck();
        }
    }

    // Buy pack button
    Rectangle packBtn = {SCREEN_W/2-100, 260, 200, 40};
    if(CheckCollisionPointRec(mouse, packBtn) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        if(g_playerCoins >= 10) {
            g_playerCoins -= 10;
            // Add 10 random cards
            for(int i=0;i<10;i++) {
                int cardId = ALL_CARDS[rand()%NUM_ALL_CARDS].id;
                if(g_collectionSize < MAX_COLLECTION)
                    g_collection[g_collectionSize++] = cardId;
            }
        }
    }

    // Scroll collection
    if(IsKeyDown(KEY_DOWN)) g_shopScroll++;
    if(IsKeyDown(KEY_UP) && g_shopScroll>0) g_shopScroll--;

    // Exit shop
    if(IsKeyPressed(KEY_ESCAPE)) {
        g_scene = SCENE_TENT3;
    }
}

static void DrawShopScene() {
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, {50,40,28,255});
    DrawText("MERCHANT'S CARD SHOP", SCREEN_W/2-120, 20, 20, {255,220,120,255});

    char buf[128];
    snprintf(buf, 128, "Your Coins: %d    Collection: %d cards    Deck: %d cards", g_playerCoins, g_collectionSize, g_playerDeckSize);
    DrawText(buf, SCREEN_W/2-200, 60, 12, {200,180,140,255});

    // Starter deck button
    if(!g_hasStarterDeck) {
        DrawRectangleRounded({(float)SCREEN_W/2-100, 200, 200, 40}, 0.3f, 4, {60,120,60,255});
        DrawText("Get Starter Deck (FREE)", SCREEN_W/2-85, 212, 12, {255,255,200,255});
    } else {
        DrawText("Starter deck acquired!", SCREEN_W/2-80, 210, 12, {120,200,120,200});
    }

    // Pack button
    Color packCol = (g_playerCoins >= 10) ? Color{80,80,140,255} : Color{50,50,60,255};
    DrawRectangleRounded({(float)SCREEN_W/2-100, 260, 200, 40}, 0.3f, 4, packCol);
    DrawText("Buy Card Pack (10 coins)", SCREEN_W/2-85, 272, 12, {255,255,200,255});

    // Collection display
    DrawText("Your Collection:", 30, 320, 14, {220,200,160,255});
    int startIdx = g_shopScroll * 8;
    int y = 340;
    for(int i=startIdx;i<g_collectionSize && y < SCREEN_H-30;i++) {
        const CardDef& cd = GetCard(g_collection[i]);
        snprintf(buf, 128, "%s (Cost:%d %s %s)", cd.name, cd.cost,
            cd.isUnit ? cd.subtype : "Support",
            cd.isUnit ? "" : "");
        DrawText(buf, 40, y, 10, {180,170,140,220});
        y += 14;
    }

    DrawText("[Esc] Exit shop    [UP/DOWN] Scroll", 20, SCREEN_H-25, 10, {150,130,100,150});
}

// ═══════════════════════════════════════════════════════════════════════════════
// CARD FRAME GENERATION — Pixel-art card textures (96x128)
// ═══════════════════════════════════════════════════════════════════════════════
static constexpr int CARD_TEX_W = 96;
static constexpr int CARD_TEX_H = 128;
static Texture2D g_cardTextures[80]; // one per card ID (up to 75 + extra)
static int g_numCardTextures = 0;

// Desert-themed 16-color pixel palette
static const Color PALETTE[] = {
    {30,22,15,255},     // 0: dark bg
    {60,45,25,255},     // 1: dark brown frame
    {110,85,50,255},    // 2: mid brown
    {165,130,70,255},   // 3: gold
    {210,175,90,255},   // 4: bright gold
    {240,220,150,255},  // 5: cream
    {255,245,200,255},  // 6: white cream
    {180,50,40,255},    // 7: red (atk)
    {60,80,160,255},    // 8: blue (def)
    {60,150,80,255},    // 9: green (support)
    {130,70,150,255},   // 10: purple (demon)
    {200,160,80,255},   // 11: tan
    {80,60,40,255},     // 12: earth brown
    {45,35,20,255},     // 13: near black
    {220,200,100,255},  // 14: gold highlight
    {140,120,80,255},   // 15: muted gold
};

// Get subtype color for card art area
static Color SubtypeColor(const char* sub) {
    if(!sub||!sub[0]) return PALETTE[9]; // support = green
    if(strstr(sub,"rat")||strstr(sub,"beast")) return {160,120,70,255};
    if(strstr(sub,"plant")) return {70,130,60,255};
    if(strstr(sub,"golem")) return {130,130,140,255};
    if(strstr(sub,"bug")) return {100,140,60,255};
    if(strstr(sub,"berserker")) return {180,60,50,255};
    if(strstr(sub,"mage")) return {80,70,160,255};
    if(strstr(sub,"angel")||strstr(sub,"spirit")) return {180,180,220,255};
    if(strstr(sub,"merchant")) return {190,170,80,255};
    if(strstr(sub,"demon")) return {120,50,140,255};
    if(strstr(sub,"zombie")) return {90,100,70,255};
    if(strstr(sub,"rogue")||strstr(sub,"soldier")) return {140,110,80,255};
    if(strstr(sub,"wurm")) return {140,100,60,255};
    return {120,100,70,255};
}

// Generate a unique pixel-art creature based on card id (deterministic hash art)
static void DrawCardCreature(Image* img, int cardId, Color baseCol, int ox, int oy, int w, int h) {
    unsigned seed = (unsigned)(cardId * 2654435761u);
    int bw = w/3 + (seed%3);
    int bh = h/3 + ((seed>>4)%4);
    int bx = ox + (w-bw)/2;
    int by = oy + h - bh - 2;
    Color body = baseCol;
    Color dark = {(unsigned char)(body.r*0.6f),(unsigned char)(body.g*0.6f),(unsigned char)(body.b*0.6f),255};
    Color light = {(unsigned char)Clamp(body.r*1.3f,0,255),(unsigned char)Clamp(body.g*1.3f,0,255),(unsigned char)Clamp(body.b*1.3f,0,255),255};
    ImageDrawRectangle(img, bx, by, bw, bh, body);
    ImageDrawRectangle(img, bx, by, bw, 1, light);
    ImageDrawRectangle(img, bx, by+bh-1, bw, 1, dark);
    int hw2 = bw/2 + ((seed>>8)%2);
    int hh2 = bh/2 + ((seed>>12)%2);
    int hx = bx + (bw-hw2)/2;
    int hy = by - hh2 + 1;
    ImageDrawRectangle(img, hx, hy, hw2, hh2, body);
    ImageDrawRectangle(img, hx, hy, hw2, 1, light);
    int ey = hy + hh2/3;
    ImageDrawPixel(img, hx+hw2/4, ey, PALETTE[0]);
    ImageDrawPixel(img, hx+hw2*3/4, ey, PALETTE[0]);
    if((seed>>16)&1) {
        ImageDrawRectangle(img, bx-3, by+1, 3, bh/2, dark);
        ImageDrawRectangle(img, bx+bw, by+1, 3, bh/2, dark);
    }
    if((seed>>17)&1) {
        ImageDrawRectangle(img, bx+bw, by+bh-3, 4, 2, dark);
    }
    if((seed>>18)&1) {
        ImageDrawPixel(img, hx+1, hy-1, dark);
        ImageDrawPixel(img, hx+hw2-2, hy-1, dark);
    }
    ImageDrawRectangle(img, bx+1, by+bh, 2, 3, dark);
    ImageDrawRectangle(img, bx+bw-3, by+bh, 2, 3, dark);
}

static void GenerateCardTexture(int cardIdx) {
    const CardDef& cd = ALL_CARDS[cardIdx];
    Image img = GenImageColor(CARD_TEX_W, CARD_TEX_H, PALETTE[0]);

    // Outer frame (2px gold border)
    ImageDrawRectangleLines(&img, {0,0,(float)CARD_TEX_W,(float)CARD_TEX_H}, 2, PALETTE[3]);
    ImageDrawRectangleLines(&img, {2,2,(float)(CARD_TEX_W-4),(float)CARD_TEX_H-4}, 1, PALETTE[1]);

    // Top bar: name background
    ImageDrawRectangle(&img, 3, 3, CARD_TEX_W-6, 14, PALETTE[1]);
    ImageDrawText(&img, cd.name, 16, 5, 8, PALETTE[5]);

    // Cost circle (top-left)
    Color costBg = cd.isUnit ? PALETTE[3] : PALETTE[9];
    ImageDrawRectangle(&img, 4, 4, 11, 11, costBg);
    ImageDrawRectangleLines(&img, {4,4,11,11}, 1, PALETTE[0]);
    char costStr[4]; snprintf(costStr, 4, "%d", cd.cost);
    ImageDrawText(&img, costStr, 6, 5, 8, PALETTE[6]);

    // Art area (top half below name bar)
    int artY = 18;
    int artH = 50;
    Color artBg = cd.isUnit ? SubtypeColor(cd.subtype) : Color{60,90,50,255};
    Color artBgDk = {(unsigned char)(artBg.r*0.4f),(unsigned char)(artBg.g*0.4f),(unsigned char)(artBg.b*0.4f),255};
    ImageDrawRectangle(&img, 4, artY, CARD_TEX_W-8, artH, artBgDk);
    ImageDrawRectangleLines(&img, {4,(float)artY,(float)(CARD_TEX_W-8),(float)artH}, 1, PALETTE[2]);

    // Draw creature/spell art
    if(cd.isUnit) {
        DrawCardCreature(&img, cd.id, artBg, 8, artY+4, CARD_TEX_W-16, artH-8);
    } else {
        int cx2 = CARD_TEX_W/2, cy2 = artY + artH/2;
        for(int a=0; a<8; a++) {
            float angle = a * 0.785f;
            for(int r=3; r<12; r++) {
                int px = cx2 + (int)(cosf(angle)*r);
                int py = cy2 + (int)(sinf(angle)*r);
                if(px>=4 && px<CARD_TEX_W-4 && py>=artY && py<artY+artH) {
                    Color sc = (r<7) ? PALETTE[4] : PALETTE[14];
                    ImageDrawPixel(&img, px, py, sc);
                }
            }
        }
    }

    // Divider line
    int divY = artY + artH + 1;
    ImageDrawRectangle(&img, 4, divY, CARD_TEX_W-8, 1, PALETTE[3]);

    // Subtype text
    if(cd.isUnit && cd.subtype[0]) {
        char subBuf[32]; snprintf(subBuf, 32, "[%s]", cd.subtype);
        ImageDrawText(&img, subBuf, 6, divY+2, 8, PALETTE[15]);
    } else {
        ImageDrawText(&img, "[Support]", 6, divY+2, 8, PALETTE[9]);
    }

    // Rules text area
    int textY = divY + 12;
    int textH = CARD_TEX_H - textY - 18;
    ImageDrawRectangle(&img, 4, textY, CARD_TEX_W-8, textH, {40,30,18,255});
    if(cd.effect[0]) {
        char line1[48]={}, line2[48]={};
        int len = (int)strlen(cd.effect);
        if(len <= 20) {
            snprintf(line1, 47, "%s", cd.effect);
        } else {
            int brk = 20;
            while(brk > 10 && cd.effect[brk] != ' ') brk--;
            if(brk <= 10) brk = 20;
            snprintf(line1, brk+1, "%s", cd.effect);
            snprintf(line2, 47, "%s", cd.effect+brk+(cd.effect[brk]==' '?1:0));
        }
        ImageDrawText(&img, line1, 6, textY+2, 8, PALETTE[5]);
        if(line2[0]) ImageDrawText(&img, line2, 6, textY+11, 8, PALETTE[5]);
    }

    // Stat blocks (units only)
    if(cd.isUnit) {
        int statY = CARD_TEX_H - 16;
        ImageDrawRectangle(&img, 3, statY, 22, 14, {140,40,30,255});
        ImageDrawRectangleLines(&img, {3,(float)statY,22,14}, 1, PALETTE[0]);
        char atkStr[8]; snprintf(atkStr, 8, "%d", cd.atk);
        ImageDrawText(&img, atkStr, 7, statY+3, 8, PALETTE[6]);
        ImageDrawPixel(&img, 5, statY+2, PALETTE[4]);
        ImageDrawPixel(&img, 5, statY+3, PALETTE[4]);

        ImageDrawRectangle(&img, CARD_TEX_W-25, statY, 22, 14, {40,50,140,255});
        ImageDrawRectangleLines(&img, {(float)(CARD_TEX_W-25),(float)statY,22,14}, 1, PALETTE[0]);
        char defStr[8]; snprintf(defStr, 8, "%d", cd.def);
        ImageDrawText(&img, defStr, CARD_TEX_W-21, statY+3, 8, PALETTE[6]);
        ImageDrawPixel(&img, CARD_TEX_W-23, statY+2, PALETTE[8]);
        ImageDrawPixel(&img, CARD_TEX_W-23, statY+3, PALETTE[8]);
    } else {
        int statY = CARD_TEX_H - 14;
        char cstr[16]; snprintf(cstr, 16, "Cost: %d", cd.cost);
        ImageDrawText(&img, cstr, CARD_TEX_W/2-20, statY+2, 8, PALETTE[15]);
    }

    // Keywords strip (above stats)
    if(cd.keywords[0]) {
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
    for(int i = 0; i < NUM_ALL_CARDS; i++) {
        GenerateCardTexture(i);
    }
    g_numCardTextures = NUM_ALL_CARDS;
}

// ═══════════════════════════════════════════════════════════════════════════════
// NPC DIALOG
// ═══════════════════════════════════════════════════════════════════════════════
static void DrawNPCDialog() {
    if (!g_npcDialogOpen) return;

    const int width = 200;
    const int height = 140;
    const int x = SCREEN_W / 2 - width / 2;
    const int y = SCREEN_H / 2 - height / 2;

    DrawRectangle(x, y, width, height, Fade(BLACK, 0.8f));
    DrawRectangleLines(x, y, width, height, DARKGRAY);

    if (g_targetNPC != -1) {
        DrawText(g_npcs[g_targetNPC].name, x + 10, y + 10, 20, WHITE);
    }

    const char* options[] = { "Talk", "Trade", "Duel", "Close" };
    for (int i = 0; i < 4; i++) {
        Color color = (i == g_dialogSelection) ? YELLOW : WHITE;
        DrawText(options[i], x + 20, y + 40 + i * 20, 20, color);
    }
}

static void UpdateNPCDialog() {
    if (!g_npcDialogOpen) return;

    if (IsKeyPressed(KEY_UP)) {
        g_dialogSelection--;
        if (g_dialogSelection < 0) g_dialogSelection = 3;
    }
    if (IsKeyPressed(KEY_DOWN)) {
        g_dialogSelection++;
        if (g_dialogSelection > 3) g_dialogSelection = 0;
    }

    if (IsKeyPressed(KEY_ENTER)) {
        switch (g_dialogSelection) {
            case 0: // Talk
                // Simple implementation: just show a message
                snprintf(g_match.message, 128, "Hello there, traveler!");
                g_match.messageTimer = 2.0f;
                g_npcDialogOpen = false;
                break;
            case 1: // Trade
                if (g_targetNPC == 2) { // Merchant Yara
                    g_scene = SCENE_SHOP;
                    g_npcDialogOpen = false;
                } else {
                    snprintf(g_match.message, 128, "I have nothing to trade.");
                    g_match.messageTimer = 2.0f;
                    g_npcDialogOpen = false;
                }
                break;
            case 2: // Duel
                if (!g_hasStarterDeck) {
                    snprintf(g_match.message, 128, "You need a deck to duel!");
                    g_match.messageTimer = 2.0f;
                } else if (g_playerDeckSize >= 20) {
                    StartMatch(g_targetNPC);
                    g_scene = SCENE_MATCH;
                } else {
                    snprintf(g_match.message, 128, "Your deck is not complete!");
                    g_match.messageTimer = 2.0f;
                }
                g_npcDialogOpen = false;
                break;
            case 3: // Close
                g_npcDialogOpen = false;
                break;
        }
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        g_npcDialogOpen = false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// NPC DIALOG
// ═══════════════════════════════════════════════════════════════════════════════
static void DrawNPCDialog() {
    if (!g_npcDialogOpen) return;

    const int width = 200;
    const int height = 140;
    const int x = SCREEN_W / 2 - width / 2;
    const int y = SCREEN_H / 2 - height / 2;

    DrawRectangle(x, y, width, height, Fade(BLACK, 0.8f));
    DrawRectangleLines(x, y, width, height, DARKGRAY);

    if (g_targetNPC != -1) {
        DrawText(g_npcs[g_targetNPC].name, x + 10, y + 10, 20, WHITE);
    }

    const char* options[] = { "Talk", "Trade", "Duel", "Close" };
    for (int i = 0; i < 4; i++) {
        Color color = (i == g_dialogSelection) ? YELLOW : WHITE;
        DrawText(options[i], x + 20, y + 40 + i * 20, 20, color);
    }
}

static void UpdateNPCDialog() {
    if (!g_npcDialogOpen) return;

    if (IsKeyPressed(KEY_UP)) {
        g_dialogSelection--;
        if (g_dialogSelection < 0) g_dialogSelection = 3;
    }
    if (IsKeyPressed(KEY_DOWN)) {
        g_dialogSelection++;
        if (g_dialogSelection > 3) g_dialogSelection = 0;
    }

    if (IsKeyPressed(KEY_ENTER)) {
        switch (g_dialogSelection) {
            case 0: // Talk
                // Simple implementation: just show a message
                snprintf(g_match.message, 128, "Hello there, traveler!");
                g_match.messageTimer = 2.0f;
                g_npcDialogOpen = false;
                break;
            case 1: // Trade
                if (g_targetNPC == 2) { // Merchant Yara
                    g_scene = SCENE_SHOP;
                    g_npcDialogOpen = false;
                } else {
                    snprintf(g_match.message, 128, "I have nothing to trade.");
                    g_match.messageTimer = 2.0f;
                    g_npcDialogOpen = false;
                }
                break;
            case 2: // Duel
                if (!g_hasStarterDeck) {
                    snprintf(g_match.message, 128, "You need a deck to duel!");
                    g_match.messageTimer = 2.0f;
                } else if (g_playerDeckSize >= 20) {
                    StartMatch(g_targetNPC);
                    g_scene = SCENE_MATCH;
                } else {
                    snprintf(g_match.message, 128, "Your deck is not complete!");
                    g_match.messageTimer = 2.0f;
                }
                g_npcDialogOpen = false;
                break;
            case 3: // Close
                g_npcDialogOpen = false;
                break;
        }
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        g_npcDialogOpen = false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// MENU OVERLAY — Desert Theme Collection Browser & Deckbuilder
// ═══════════════════════════════════════════════════════════════════════════════
static void UpdateMenu() {
    // Tab switching
    if(IsKeyPressed(KEY_TAB) || IsKeyPressed(KEY_RIGHT))
        g_menuTab = (MenuTab)((g_menuTab+1)%TAB_COUNT);
    if(IsKeyPressed(KEY_LEFT) && g_menuTab > 0)
        g_menuTab = (MenuTab)(g_menuTab-1);

    // Scrolling
    int wheel = (int)GetMouseWheelMove();
    if(g_menuTab == TAB_COLLECTION) {
        g_collScroll -= wheel;
        if(g_collScroll < 0) g_collScroll = 0;
        int maxScroll = (g_numCardCopies / 3) - 3;
        if(maxScroll < 0) maxScroll = 0;
        if(g_collScroll > maxScroll) g_collScroll = maxScroll;
    } else {
        g_deckScroll -= wheel;
        if(g_deckScroll < 0) g_deckScroll = 0;
    }

    // Close menu
    if(IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
        g_menuOpen = false;
        return;
    }
}

static void DrawMenuOverlay() {
    if(!g_menuOpen) return;
    RebuildCardCopies();
    UpdateMenu();

    // Semi-transparent dark overlay
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, {20,15,8,200});

    // Menu frame
    int mx = 30, my = 20, mw = SCREEN_W-60, mh = SCREEN_H-40;
    DrawRectangleRounded({(float)mx,(float)my,(float)mw,(float)mh}, 0.02f, 4, {45,35,20,250});
    DrawRectangleRoundedLines({(float)mx,(float)my,(float)mw,(float)mh}, 0.02f, 4, {210,175,90,255});
    DrawRectangleRoundedLines({(float)(mx+2),(float)(my+2),(float)(mw-4),(float)(mh-4)}, 0.02f, 4, {140,110,60,180});

    // Title bar
    DrawRectangle(mx+4, my+4, mw-8, 28, {60,45,25,255});
    DrawText("SOVEREIGN HORIZONS", mx+mw/2-80, my+10, 16, {220,190,100,255});

    // Tab buttons
    int tabY = my+36;
    const char* tabNames[] = {"Collection", "Deck Builder"};
    for(int t=0; t<TAB_COUNT; t++) {
        int tabX = mx+10+t*120;
        bool active = (g_menuTab==(MenuTab)t);
        Color bg = active ? Color{110,85,50,255} : Color{60,45,25,255};
        Color fg = active ? Color{240,220,150,255} : Color{160,140,100,255};
        DrawRectangle(tabX, tabY, 110, 22, bg);
        DrawRectangleLines(tabX, tabY, 110, 22, {140,110,60,200});
        DrawText(tabNames[t], tabX+8, tabY+5, 12, fg);
        if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            Vector2 mp = GetMousePosition();
            if(mp.x>=tabX && mp.x<tabX+110 && mp.y>=tabY && mp.y<tabY+22)
                g_menuTab = (MenuTab)t;
        }
    }

    int panelY = tabY + 30;
    int panelH = mh - (panelY - my) - 10;

    if(g_menuTab == TAB_COLLECTION) {
        // COLLECTION BROWSER: 3x4 grid of card thumbnails
        DrawText("Your Collection", mx+15, panelY, 14, {200,175,100,255});

        int cardW = 100, cardH = 136;
        int gapX = 12, gapY = 10;
        int gridX = mx+15, gridY = panelY+22;
        int cols = 3, rows = 4;

        // Left panel: card grid
        for(int row=0; row<rows; row++) {
            for(int col=0; col<cols; col++) {
                int idx = (g_collScroll + row) * cols + col;
                if(idx >= g_numCardCopies) continue;

                int cx2 = gridX + col*(cardW+gapX);
                int cy2 = gridY + row*(cardH+gapY);
                int cid = g_cardCopies[idx].cardId;

                // Draw card texture
                if(cid > 0 && cid < 80 && g_cardTextures[cid].id) {
                    Rectangle src = {0,0,(float)CARD_TEX_W,(float)CARD_TEX_H};
                    Rectangle dst = {(float)cx2,(float)cy2,(float)cardW,(float)cardH};
                    DrawTexturePro(g_cardTextures[cid], src, dst, {0,0}, 0, WHITE);
                }

                // Highlight selected
                if(g_selectedCardId == cid) {
                    DrawRectangleLinesEx({(float)cx2-1,(float)cy2-1,(float)(cardW+2),(float)(cardH+2)}, 2, {255,220,80,255});
                }

                // Copy count below card
                char countStr[16]; snprintf(countStr, 16, "x%d", g_cardCopies[idx].count);
                int tw = MeasureText(countStr, 10);
                DrawText(countStr, cx2+cardW/2-tw/2, cy2+cardH+2, 10, {200,175,100,200});

                // Click to select
                Vector2 mp = GetMousePosition();
                if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
                   mp.x>=cx2 && mp.x<cx2+cardW && mp.y>=cy2 && mp.y<cy2+cardH) {
                    g_selectedCardId = cid;
                }
            }
        }

        // Right panel: card detail
        int detX = gridX + cols*(cardW+gapX) + 20;
        int detW = mx+mw-10-detX;
        DrawRectangle(detX, gridY, detW, panelH-30, {35,28,18,220});
        DrawRectangleLines(detX, gridY, detW, panelH-30, {140,110,60,180});

        if(g_selectedCardId > 0) {
            const CardDef& cd = GetCard(g_selectedCardId);
            // Large card preview
            if(g_cardTextures[g_selectedCardId].id) {
                Rectangle src = {0,0,(float)CARD_TEX_W,(float)CARD_TEX_H};
                Rectangle dst = {(float)(detX+detW/2-72),(float)(gridY+8),144,192};
                DrawTexturePro(g_cardTextures[g_selectedCardId], src, dst, {0,0}, 0, WHITE);
            }
            // Card details below
            int ty = gridY + 208;
            DrawText(cd.name, detX+10, ty, 14, {240,220,150,255}); ty+=20;
            char info[64];
            if(cd.isUnit) {
                snprintf(info, 64, "Cost: %d  ATK: %d  DEF: %d", cd.cost, cd.atk, cd.def);
            } else {
                snprintf(info, 64, "Support - Cost: %d", cd.cost);
            }
            DrawText(info, detX+10, ty, 11, {180,160,110,255}); ty+=16;
            if(cd.isUnit && cd.subtype[0]) {
                DrawText(cd.subtype, detX+10, ty, 10, {160,140,100,200}); ty+=14;
            }
            if(cd.keywords[0]) {
                DrawText(cd.keywords, detX+10, ty, 10, {200,180,80,220}); ty+=14;
            }
            if(cd.effect[0]) {
                DrawText(cd.effect, detX+10, ty, 10, {180,170,130,220}); ty+=14;
            }

            // "Add to Deck" button
            int btnY = ty + 10;
            DrawRectangle(detX+10, btnY, 100, 22, {80,60,35,255});
            DrawRectangleLines(detX+10, btnY, 100, 22, {180,150,80,200});
            DrawText("[A] Add to Deck", detX+14, btnY+5, 10, {220,200,120,255});

            // Handle add-to-deck
            if(IsKeyPressed(KEY_A) || (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
               GetMousePosition().x >= detX+10 && GetMousePosition().x < detX+110 &&
               GetMousePosition().y >= btnY && GetMousePosition().y < btnY+22)) {
                if(g_playerDeckSize < MAX_DECK) {
                    g_playerDeck[g_playerDeckSize++] = g_selectedCardId;
                }
            }
        }

        DrawText("Scroll: Mouse Wheel", mx+15, my+mh-20, 10, {140,120,80,150});

    } else {
        // DECK BUILDER: Split screen
        int leftW = mw/2 - 20;
        DrawRectangle(mx+10, panelY, leftW, panelH-10, {35,28,18,220});
        DrawRectangleLines(mx+10, panelY, leftW, panelH-10, {140,110,60,180});

        char deckTitle[64]; snprintf(deckTitle, 64, "Your Deck (%d/%d cards)", g_playerDeckSize, MAX_DECK);
        DrawText(deckTitle, mx+18, panelY+6, 13, {220,190,100,255});

        // List deck cards
        int listY = panelY + 24;
        int itemH = 18;
        int visible = (panelH-40) / itemH;
        for(int i = g_deckScroll; i < g_playerDeckSize && (i-g_deckScroll) < visible; i++) {
            int iy = listY + (i-g_deckScroll)*itemH;
            const CardDef& cd = GetCard(g_playerDeck[i]);
            bool hovered = false;
            Vector2 mp = GetMousePosition();
            if(mp.x >= mx+12 && mp.x < mx+10+leftW-4 && mp.y >= iy && mp.y < iy+itemH) {
                hovered = true;
                DrawRectangle(mx+12, iy, leftW-4, itemH, {60,45,25,180});
            }
            Color tc = cd.isUnit ? Color{200,180,120,255} : Color{120,180,120,255};
            char entry[80]; snprintf(entry, 80, "%d. [%d] %s", i+1, cd.cost, cd.name);
            DrawText(entry, mx+18, iy+3, 10, tc);

            // Click to remove from deck
            if(hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                for(int j=i; j<g_playerDeckSize-1; j++) g_playerDeck[j]=g_playerDeck[j+1];
                g_playerDeckSize--;
                if(g_deckScroll > 0 && g_deckScroll >= g_playerDeckSize) g_deckScroll--;
            }
        }

        // Remove hint
        DrawText("Click card to remove  |  [B] Remove last", mx+18, panelY+panelH-22, 9, {160,140,100,160});
        if(IsKeyPressed(KEY_B) && g_playerDeckSize > 0) g_playerDeckSize--;

        // Right: Card pool (collection browser)
        int rightX = mx+10+leftW+10;
        int rightW = mw - leftW - 30;
        DrawRectangle(rightX, panelY, rightW, panelH-10, {35,28,18,220});
        DrawRectangleLines(rightX, panelY, rightW, panelH-10, {140,110,60,180});
        DrawText("Card Pool", rightX+8, panelY+6, 13, {220,190,100,255});

        // Compact card list (pool)
        int poolY = panelY + 24;
        int poolVisible = (panelH-40) / itemH;
        for(int i = 0; i < g_numCardCopies && i < poolVisible; i++) {
            int iy = poolY + i*itemH;
            const CardDef& cd = GetCard(g_cardCopies[i].cardId);
            bool hovered = false;
            Vector2 mp = GetMousePosition();
            if(mp.x >= rightX+4 && mp.x < rightX+rightW-4 && mp.y >= iy && mp.y < iy+itemH) {
                hovered = true;
                DrawRectangle(rightX+4, iy, rightW-8, itemH, {60,45,25,180});
                if(cd.effect[0]) {
                    DrawRectangle((int)mp.x+10, (int)mp.y-30, 200, 28, {30,25,15,240});
                    DrawRectangleLines((int)mp.x+10, (int)mp.y-30, 200, 28, {140,110,60,200});
                    DrawText(cd.effect, (int)mp.x+14, (int)mp.y-26, 8, {200,180,130,255});
                }
            }
            Color tc = cd.isUnit ? Color{200,180,120,255} : Color{120,180,120,255};
            char entry[80];
            if(cd.isUnit)
                snprintf(entry, 80, "[%d] %s %d/%d x%d", cd.cost, cd.name, cd.atk, cd.def, g_cardCopies[i].count);
            else
                snprintf(entry, 80, "[%d] %s x%d", cd.cost, cd.name, g_cardCopies[i].count);
            DrawText(entry, rightX+10, iy+3, 10, tc);

            // Click to add to deck
            if(hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                if(g_playerDeckSize < MAX_DECK) {
                    g_playerDeck[g_playerDeckSize++] = g_cardCopies[i].cardId;
                }
            }
        }
    }

    // Bottom status bar
    char statusStr[128];
    snprintf(statusStr, 128, "Coins: %d  |  Collection: %d cards  |  Deck: %d/%d  |  Tab/Arrows: Switch  |  Esc: Close",
             g_playerCoins, g_collectionSize, g_playerDeckSize, MAX_DECK);
    DrawText(statusStr, mx+15, my+mh-8, 9, {160,140,100,180});
}

// ═══════════════════════════════════════════════════════════════════════════════
// HD-2D SPRITE GENERATION — Pokémon BW-style volumetric pixel art (24×32)
// 3-tone shading per element, 1px outline, dynamic walk cycle
// ═══════════════════════════════════════════════════════════════════════════════
static const Color BK=BLANK;
static void Px(Image*img,int x,int y,Color c){if(x>=0&&x<SPR_W&&y>=0&&y<SPR_H&&c.a>0)ImageDrawPixel(img,x,y,c);}
static void PxR(Image*img,int x,int y,int w,int h,Color c){for(int py=y;py<y+h;py++)for(int px=x;px<x+w;px++)Px(img,px,py,c);}
// Outline: scan all pixels, add dark border around non-transparent
static void AddOutline(Image*img,Color ol){
    Image tmp=ImageCopy(*img);
    for(int y=0;y<SPR_H;y++)for(int x=0;x<SPR_W;x++){
        Color c=GetImageColor(tmp,x,y);
        if(c.a<10){
            // Check neighbors — if any neighbor is opaque, draw outline
            for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
                if(dx==0&&dy==0)continue;
                int nx=x+dx,ny=y+dy;
                if(nx>=0&&nx<SPR_W&&ny>=0&&ny<SPR_H){
                    Color nc=GetImageColor(tmp,nx,ny);
                    if(nc.a>128){Px(img,x,y,ol);goto done;}
                }
            }
            done:;
        }
    }
    UnloadImage(tmp);
}
// Color math helpers
static Color Cblend(Color a,Color b,float t){
    return{(unsigned char)(a.r+(b.r-a.r)*t),(unsigned char)(a.g+(b.g-a.g)*t),
           (unsigned char)(a.b+(b.b-a.b)*t),255};
}
static Color Cdark(Color c,float f){return{(unsigned char)(c.r*f),(unsigned char)(c.g*f),(unsigned char)(c.b*f),255};}
static Color Clight(Color c,int d){return{(unsigned char)Clamp(c.r+d,0,255),(unsigned char)Clamp(c.g+d,0,255),(unsigned char)Clamp(c.b+d,0,255),255};
}

// ── Character definition for reusable palette ─────────────────────────────────
struct CharPalette {
    Color capFront, capBack, capBrim;     // Hat/cap colors
    Color jacket, jacketHi, jacketSh;     // Torso
    Color stripe;                          // Jacket accent stripe
    Color pants, pantsSh;                  // Legs
    Color shoe, shoeSh;                    // Feet
    Color skin, skinHi, skinSh;           // Skin tones
    Color hair;                            // Hair
    Color eye;                             // Eyes
    Color outline;                         // Dark outline
};

static const CharPalette g_playerPal = {
    {200,40,40,255},{240,240,245,255},{150,25,25,255},       // Red/white cap
    {60,90,180,255},{90,120,210,255},{40,60,140,255},         // Blue jacket
    {240,240,245,255},                                        // White stripe
    {45,42,58,255},{30,28,42,255},                            // Dark pants
    {180,50,40,255},{130,35,28,255},                          // Red shoes
    {222,178,130,255},{240,200,155,255},{190,145,105,255},    // Skin
    {50,35,25,255},                                           // Hair
    {20,20,30,255},                                           // Eyes
    {25,20,18,255}                                            // Outline
};

// ── Down-facing sprite (walking toward camera) ────────────────────────────────
static Image GenBWSprDown(int f, const CharPalette& p){
    Image img=GenImageColor(SPR_W,SPR_H,BK);
    int lo=(f==1)?1:(f==2)?-1:0;  // leg offset for walk cycle

    // ── Cap (rows 1-5) ────────────────────────────
    PxR(&img,7,1,10,2,p.capFront);                      // cap top (red front)
    PxR(&img,13,1,4,2,p.capBack);                        // white back portion
    PxR(&img,6,3,12,2,Cdark(p.capFront,0.85f));          // cap mid
    PxR(&img,12,3,5,2,Cdark(p.capBack,0.92f));           // white mid
    PxR(&img,5,5,14,1,p.capBrim);                        // brim (dark visor)
    // Cap highlight
    Px(&img,8,1,Clight(p.capFront,25));Px(&img,9,1,Clight(p.capFront,25));

    // ── Hair (peeks out sides) ────────────────────
    PxR(&img,5,4,2,2,p.hair);PxR(&img,17,4,2,2,p.hair);
    PxR(&img,6,6,1,1,p.hair);PxR(&img,17,6,1,1,p.hair);

    // ── Face (rows 6-10) ──────────────────────────
    PxR(&img,7,6,10,5,p.skin);                           // face base
    PxR(&img,7,6,10,1,p.skinHi);                         // forehead highlight
    PxR(&img,7,10,10,1,p.skinSh);                        // chin shadow
    // Eyes (row 8)
    PxR(&img,8,8,2,2,p.eye);PxR(&img,14,8,2,2,p.eye);
    Px(&img,9,8,{255,255,255,255});Px(&img,15,8,{255,255,255,255}); // eye highlights
    // Mouth hint
    Px(&img,11,10,p.skinSh);Px(&img,12,10,p.skinSh);

    // ── Neck ──────────────────────────────────────
    PxR(&img,9,11,6,1,p.skinSh);

    // ── Jacket torso (rows 12-19) ─────────────────
    PxR(&img,6,12,12,8,p.jacket);                        // base jacket
    PxR(&img,6,12,12,1,p.jacketHi);                      // collar highlight
    PxR(&img,6,12,2,8,p.jacketSh);                       // left shadow
    PxR(&img,16,12,2,8,p.jacketHi);                      // right highlight (sun side)
    // White stripe across chest
    PxR(&img,8,15,8,1,p.stripe);
    PxR(&img,8,16,8,1,Cdark(p.stripe,0.9f));
    // Arms
    PxR(&img,4,13+((f==1)?-1:0),2,6,p.jacketSh);        // left arm
    PxR(&img,18,13+((f==2)?-1:0),2,6,p.jacketHi);       // right arm
    // Arm skin (hands)
    PxR(&img,4,18+((f==1)?-1:0),2,2,p.skin);
    PxR(&img,18,18+((f==2)?-1:0),2,2,p.skin);
