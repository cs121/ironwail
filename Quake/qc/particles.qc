// particles.qc – retro-minimal particle helpers for Ironwail
// Maps to effectinfo.txt names from "Retro Plus MAX Transparent" preset.
// Falls back to classic TempEntities / particle() when effectinfo/pointparticles are unavailable.

// -------------------------------
// EFFECT HANDLE CACHE
// -------------------------------
float FX_INVALID = -1;

// Core minimal
float FX_SPARK = -1;         // fx_spark_min
float FX_SMOKE = -1;         // fx_smoke_min
float FX_BLOOD = -1;         // fx_blood_spray
float FX_GIB = -1;           // fx_gib_chunk
float FX_LIGHTNING = -1;     // fx_lightning_hit
float FX_EXPLOSION = -1;     // fx_explosion_burst

// Explosions (3-Layer)
float FX_EXP_CORE = -1;      // te_explosion_core
float FX_EXP_MID = -1;       // te_explosion_mid
float FX_EXP_OUT = -1;       // te_explosion_outer
float FX_EXP2_CORE = -1;     // te_explosion2_core
float FX_EXP2_MID  = -1;     // te_explosion2_mid
float FX_EXP2_OUT  = -1;     // te_explosion2_outer
float FX_TAR_SMOKE = -1;     // te_tarexplosion

// Impacts / materials
float FX_IMPACT_DIRT  = -1;  // fx_impact_dirt
float FX_IMPACT_STONE = -1;  // fx_impact_stone
float FX_IMPACT_METAL = -1;  // fx_impact_metal
float FX_GUNSHOT_SMK  = -1;  // te_gunshot
float FX_SPIKE        = -1;  // te_spike
float FX_SUPERSPIKE   = -1;  // te_superspike
float FX_WIZSPIKE     = -1;  // te_wizspike
float FX_KNIGHTSPIKE  = -1;  // te_knightspike

// Trails / muzzle
float FX_ROCKET_SMK   = -1;  // fx_rocket_trail_smoke
float FX_ROCKET_EMBER = -1;  // fx_rocket_trail_embers
float FX_GRENADE_TRL  = -1;  // fx_grenade_trail
float FX_NAIL_TRL     = -1;  // fx_nail_trail
float FX_VORE_TRL     = -1;  // fx_voreball_trail
float FX_MUZZLE_SMK   = -1;  // fx_muzzle_smoke
float FX_MUZZLE_SPK   = -1;  // fx_muzzle_sparks

// Lightning / misc
float FX_LIGHTNING_RES = -1; // fx_lightning_residual
float FX_TP_SMOKE      = -1; // te_teleport
float FX_LAVASPLASH    = -1; // te_lavasplash

// Dust / debris / bubbles
float FX_DUST_CLOUD  = -1;   // fx_dust_cloud
float FX_DEBRIS      = -1;   // fx_debris_chunk
float FX_SMOKE_PUFF  = -1;   // fx_smoke_puff_min
float FX_SPARK_SHWR  = -1;   // fx_spark_shower
float FX_BUBBLE_TRL  = -1;   // fx_bubble_trail
float FX_LAVA_BUBBLE = -1;   // fx_lava_bubble

float particles_supported = 0;

// Optional overrides (uncomment if your defs.qc doesn’t define them)
// float MSG_BROADCAST = 2;
// float SVC_TEMPENTITY = 23;
// float TE_GUNSHOT = 2;
// float TE_EXPLOSION = 3;

float(string name) Particles_Fetch =
{
    local float fx;
    fx = particleeffectnum(name);
    if (fx < 0)
        return FX_INVALID;
    return fx;
};

// -------------------------------
// INIT / FETCH
// -------------------------------
void() Particles_Init =
{
    particles_supported = 0;

    // Clear all handles
    FX_SPARK = FX_SMOKE = FX_BLOOD = FX_GIB = FX_LIGHTNING = FX_EXPLOSION = FX_INVALID;
    FX_EXP_CORE = FX_EXP_MID = FX_EXP_OUT = FX_EXP2_CORE = FX_EXP2_MID = FX_EXP2_OUT = FX_INVALID;
    FX_TAR_SMOKE = FX_IMPACT_DIRT = FX_IMPACT_STONE = FX_IMPACT_METAL = FX_INVALID;
    FX_GUNSHOT_SMK = FX_SPIKE = FX_SUPERSPIKE = FX_WIZSPIKE = FX_KNIGHTSPIKE = FX_INVALID;
    FX_ROCKET_SMK = FX_ROCKET_EMBER = FX_GRENADE_TRL = FX_NAIL_TRL = FX_VORE_TRL = FX_INVALID;
    FX_MUZZLE_SMK = FX_MUZZLE_SPK = FX_LIGHTNING_RES = FX_TP_SMOKE = FX_LAVASPLASH = FX_INVALID;
    FX_DUST_CLOUD = FX_DEBRIS = FX_SMOKE_PUFF = FX_SPARK_SHWR = FX_BUBBLE_TRL = FX_LAVA_BUBBLE = FX_INVALID;

    if (!checkextension("DP_QC_POINTPARTICLES"))
        return;

    particles_supported = 1;

    // Core minimal
    FX_SPARK     = Particles_Fetch("fx_spark_min");
    FX_SMOKE     = Particles_Fetch("fx_smoke_min");
    FX_BLOOD     = Particles_Fetch("fx_blood_spray");
    FX_GIB       = Particles_Fetch("fx_gib_chunk");
    FX_LIGHTNING = Particles_Fetch("fx_lightning_hit");
    FX_EXPLOSION = Particles_Fetch("fx_explosion_burst");

    // Explosions (3-Layer)
    FX_EXP_CORE  = Particles_Fetch("te_explosion_core");
    FX_EXP_MID   = Particles_Fetch("te_explosion_mid");
    FX_EXP_OUT   = Particles_Fetch("te_explosion_outer");
    FX_EXP2_CORE = Particles_Fetch("te_explosion2_core");
    FX_EXP2_MID  = Particles_Fetch("te_explosion2_mid");
    FX_EXP2_OUT  = Particles_Fetch("te_explosion2_outer");
    FX_TAR_SMOKE = Particles_Fetch("te_tarexplosion");

    // Impacts / materials
    FX_IMPACT_DIRT  = Particles_Fetch("fx_impact_dirt");
    FX_IMPACT_STONE = Particles_Fetch("fx_impact_stone");
    FX_IMPACT_METAL = Particles_Fetch("fx_impact_metal");
    FX_GUNSHOT_SMK  = Particles_Fetch("te_gunshot");
    FX_SPIKE        = Particles_Fetch("te_spike");
    FX_SUPERSPIKE   = Particles_Fetch("te_superspike");
    FX_WIZSPIKE     = Particles_Fetch("te_wizspike");
    FX_KNIGHTSPIKE  = Particles_Fetch("te_knightspike");

    // Trails / muzzle
    FX_ROCKET_SMK   = Particles_Fetch("fx_rocket_trail_smoke");
    FX_ROCKET_EMBER = Particles_Fetch("fx_rocket_trail_embers");
    FX_GRENADE_TRL  = Particles_Fetch("fx_grenade_trail");
    FX_NAIL_TRL     = Particles_Fetch("fx_nail_trail");
    FX_VORE_TRL     = Particles_Fetch("fx_voreball_trail");
    FX_MUZZLE_SMK   = Particles_Fetch("fx_muzzle_smoke");
    FX_MUZZLE_SPK   = Particles_Fetch("fx_muzzle_sparks");

    // Lightning / misc
    FX_LIGHTNING_RES = Particles_Fetch("fx_lightning_residual");
    FX_TP_SMOKE      = Particles_Fetch("te_teleport");
    FX_LAVASPLASH    = Particles_Fetch("te_lavasplash");

    // Dust / debris / bubbles
    FX_DUST_CLOUD  = Particles_Fetch("fx_dust_cloud");
    FX_DEBRIS      = Particles_Fetch("fx_debris_chunk");
    FX_SMOKE_PUFF  = Particles_Fetch("fx_smoke_puff_min");
    FX_SPARK_SHWR  = Particles_Fetch("fx_spark_shower");
    FX_BUBBLE_TRL  = Particles_Fetch("fx_bubble_trail");
    FX_LAVA_BUBBLE = Particles_Fetch("fx_lava_bubble");
};

// -------------------------------
// LOW-LEVEL EMITTERS
// -------------------------------

// Spawns point particles with optional directional bias.
// Fallback: approximate with TE_GUNSHOT bursts.
void(float fx, vector org, vector dir, float count) Particles_Point =
{
    if (count < 1)
        count = 1;

    if (particles_supported && fx >= 0)
    {
        pointparticles(fx, org, dir, count);
        return;
    }

    // Retro fallback: TE_GUNSHOT puffs
    local float i;
    i = 0;
    while (i < count)
    {
        WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        WriteByte(MSG_BROADCAST, TE_GUNSHOT);
        WriteCoord(MSG_BROADCAST, org_x);
        WriteCoord(MSG_BROADCAST, org_y);
        WriteCoord(MSG_BROADCAST, org_z);
        i = i + 1;
    }
};

void(float fx, vector org, float count) Particles_Burst =
{
    Particles_Point(fx, org, '0 0 0', count);
};

// -------------------------------
// BLOOD / GIB
// -------------------------------
void(vector org, vector dir, float count) Particles_Blood =
{
    if (count < 1) count = 1;

    if (particles_supported && FX_BLOOD >= 0)
        pointparticles(FX_BLOOD, org, dir, count);
    else
        particle(org, dir, 73, count); // classic blood
};

void(vector org, vector dir, float count) Particles_Gib =
{
    if (count < 1) count = 1;

    if (particles_supported && FX_GIB >= 0)
        pointparticles(FX_GIB, org, dir, count);
    else
        particle(org, dir, 0, count); // chunk
};

// -------------------------------
// LIGHTNING CONTACT
// -------------------------------
void(vector org, float count) Particles_LightningHit =
{
    if (count < 1) count = 1;

    if (particles_supported && FX_LIGHTNING >= 0)
        pointparticles(FX_LIGHTNING, org, '0 0 200', count);
    else
        particle(org, '0 0 100', 225, count);
};

void(vector org, float count) Particles_LightningResidual =
{
    if (count < 1) count = 1;

    if (particles_supported && FX_LIGHTNING_RES >= 0)
        pointparticles(FX_LIGHTNING_RES, org, '0 0 0', count);
};

// -------------------------------
// EXPLOSIONS
// -------------------------------

// Small fiery burst (barrels, small props)
void(vector org, float count) Particles_ExplosionBurst =
{
    if (count < 1) count = 1;

    if (particles_supported && FX_EXPLOSION >= 0)
        pointparticles(FX_EXPLOSION, org, '0 0 0', count);
    else
        particle(org, '0 0 0', 75, count);
};

// Classic TE_EXPLOSION style using 3 layered effects
void(vector org, float scale) Particles_Explosion3 =
{
    if (scale <= 0) scale = 1;

    if (particles_supported &&
        FX_EXP_CORE >= 0 && FX_EXP_MID >= 0 && FX_EXP_OUT >= 0)
    {
        pointparticles(FX_EXP_CORE, org, '0 0 0', 18 * scale);
        pointparticles(FX_EXP_MID,  org, '0 0 0', 28 * scale);
        pointparticles(FX_EXP_OUT,  org, '0 0 0', 40 * scale);
        return;
    }

    // Fallback: one tempentity per "layer"
    local float i;
    i = 0;
    while (i < (3 * scale))
    {
        WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        WriteByte(MSG_BROADCAST, TE_EXPLOSION);
        WriteCoord(MSG_BROADCAST, org_x);
        WriteCoord(MSG_BROADCAST, org_y);
        WriteCoord(MSG_BROADCAST, org_z);
        i = i + 1;
    }
};

// Brighter variant
void(vector org, float scale) Particles_Explosion3_Bright =
{
    if (scale <= 0) scale = 1;

    if (particles_supported &&
        FX_EXP2_CORE >= 0 && FX_EXP2_MID >= 0 && FX_EXP2_OUT >= 0)
    {
        pointparticles(FX_EXP2_CORE, org, '0 0 0', 16 * scale);
        pointparticles(FX_EXP2_MID,  org, '0 0 0', 26 * scale);
        pointparticles(FX_EXP2_OUT,  org, '0 0 0', 38 * scale);
        return;
    }

    Particles_Explosion3(org, scale); // fallback reuse
};

// Tar explosion smoke (single-layer darker puff)
void(vector org, float count) Particles_TarExplosion =
{
    if (count < 1) count = 1;
    Particles_Burst(FX_TAR_SMOKE, org, count);
};

// -------------------------------
// IMPACTS
// -------------------------------
void(vector org, vector dir, float count) Particles_GunshotImpact =
{
    // prefer your te_gunshot (smoke) if available, else minimal smoke
    if (particles_supported && FX_GUNSHOT_SMK >= 0)
        pointparticles(FX_GUNSHOT_SMK, org, dir, count);
    else
        Particles_Point(FX_SMOKE, org, dir, count);
};

void(vector org, vector dir, float count) Particles_SpikeImpact =
{
    Particles_Point(FX_SPIKE, org, dir, count);
};

void(vector org, vector dir, float count) Particles_SuperSpikeImpact =
{
    Particles_Point(FX_SUPERSPIKE, org, dir, count);
};

void(vector org, vector dir, float count) Particles_WizSpikeImpact =
{
    Particles_Point(FX_WIZSPIKE, org, dir, count);
};

void(vector org, vector dir, float count) Particles_KnightSpikeImpact =
{
    Particles_Point(FX_KNIGHTSPIKE, org, dir, count);
};

// Material-specific helpers
void(vector org, vector dir, float count) Particles_ImpactDirt  = { Particles_Point(FX_IMPACT_DIRT,  org, dir, count); };
void(vector org, vector dir, float count) Particles_ImpactStone = { Particles_Point(FX_IMPACT_STONE, org, dir, count); };
void(vector org, vector dir, float count) Particles_ImpactMetal = { Particles_Point(FX_IMPACT_METAL, org, dir, count); };

// -------------------------------
// TRAILS
// -------------------------------
void(vector org, float density) Trail_Rocket_Step =
{
    if (density <= 0) density = 1;
    Particles_Burst(FX_ROCKET_SMK, org, 10 * density);
    Particles_Burst(FX_ROCKET_EMBER, org, 8 * density);
};

void(vector org, float density) Trail_Grenade_Step =
{
    if (density <= 0) density = 1;
    Particles_Burst(FX_GRENADE_TRL, org, 6 * density);
};

void(vector org, float density) Trail_Nail_Step =
{
    if (density <= 0) density = 1;
    Particles_Burst(FX_NAIL_TRL, org, 4 * density);
};

void(vector org, float density) Trail_VoreBall_Step =
{
    if (density <= 0) density = 1;
    Particles_Burst(FX_VORE_TRL, org, 10 * density);
};

// -------------------------------
// MUZZLE / TELEPORT / LAVA / BUBBLES
// -------------------------------
void(vector org) Particles_MuzzleFlash =
{
    Particles_Burst(FX_MUZZLE_SPK, org, 10);
    Particles_Burst(FX_MUZZLE_SMK, org, 12);
};

void(vector org, float count) Particles_Teleport =
{
    Particles_Burst(FX_TP_SMOKE, org, count);
};

void(vector org, float count) Particles_LavaSplash =
{
    Particles_Burst(FX_LAVASPLASH, org, count);
};

void(vector org, float count) Particles_Bubbles =
{
    Particles_Burst(FX_BUBBLE_TRL, org, count);
};

void(vector org, float count) Particles_LavaBubbles =
{
    Particles_Burst(FX_LAVA_BUBBLE, org, count);
};

// -------------------------------
// DUST / DEBRIS / MINIMAL
// -------------------------------
void(vector org, float count) Particles_DustCloud =
{
    Particles_Burst(FX_DUST_CLOUD, org, count);
};

void(vector org, float count) Particles_Debris =
{
    Particles_Burst(FX_DEBRIS, org, count);
};

void(vector org, float count) Particles_SmokePuffMin =
{
    Particles_Burst(FX_SMOKE_PUFF, org, count);
};

void(vector org, float count) Particles_SparkShower =
{
    Particles_Burst(FX_SPARK_SHWR, org, count);
};

// -------------------------------
// QUICK MAPPINGS (drop-in replacements)
// -------------------------------

void(vector org, vector dir, float count) Particles_Smoke = { Particles_Point(FX_SMOKE, org, dir, count); };
void(vector org, vector dir, float count) Particles_Sparks = { Particles_Point(FX_SPARK, org, dir, count); };
