// particles.qc – retro-minimal particle helpers for Ironwail
// Provides a compact API for mods while staying compatible with
// classic TempEntity fallbacks when effectinfo-powered particles
// are unavailable.

float FX_INVALID = -1;
float FX_SPARK = -1;
float FX_SMOKE = -1;
float FX_BLOOD = -1;
float FX_GIB = -1;
float FX_LIGHTNING = -1;
float FX_EXPLOSION = -1;

float particles_supported = 0;

// Optional overrides for message constants. Uncomment if your
// progs doesn't already define them in defs.qc.
// float MSG_BROADCAST = 2;
// float SVC_TEMPENTITY = 23;
// float TE_GUNSHOT = 2;

float(string name) Particles_FetchEffect =
{
    local float fx;
    fx = particleeffectnum(name);
    if (fx < 0)
        return FX_INVALID;
    return fx;
};

void() Particles_Init =
{
    particles_supported = 0;
    FX_SPARK = FX_SMOKE = FX_BLOOD = FX_GIB = FX_LIGHTNING = FX_EXPLOSION = FX_INVALID;

    // Detect whether the engine exposes pointparticles/effectinfo
    if (!checkextension("DP_QC_POINTPARTICLES"))
        return;

    particles_supported = 1;
    FX_SPARK = Particles_FetchEffect("fx_spark_min");
    FX_SMOKE = Particles_FetchEffect("fx_smoke_min");
    FX_BLOOD = Particles_FetchEffect("fx_blood_spray");
    FX_GIB = Particles_FetchEffect("fx_gib_chunk");
    FX_LIGHTNING = Particles_FetchEffect("fx_lightning_hit");
    FX_EXPLOSION = Particles_FetchEffect("fx_explosion_burst");
};

// Spawns point particles with optional directional bias.
// Falls back to TE_GUNSHOT bursts if the engine lacks support.
void(float fx, vector org, vector dir, float count) Particles_Point =
{
    if (particles_supported && fx >= 0)
    {
        pointparticles(fx, org, dir, count);
        return;
    }

    // Retro fallback: approximate the look with TempEntities.
    if (count < 1)
        count = 1;

    local float emitted;
    emitted = 0;
    while (emitted < count)
    {
        WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        WriteByte(MSG_BROADCAST, TE_GUNSHOT);
        WriteCoord(MSG_BROADCAST, org_x);
        WriteCoord(MSG_BROADCAST, org_y);
        WriteCoord(MSG_BROADCAST, org_z);
        emitted = emitted + 1;
    }
};

// Convenience helper that omits direction; the effect definition
// controls spread/velocity.
void(float fx, vector org, float count) Particles_Burst =
{
    Particles_Point(fx, org, '0 0 0', count);
};

// Blood spray helper used by gib/death impacts.
void(vector org, vector dir, float count) Particles_Blood =
{
    if (count < 1)
        count = 1;

    if (particles_supported && FX_BLOOD >= 0)
    {
        pointparticles(FX_BLOOD, org, dir, count);
        return;
    }

    particle(org, dir, 73, count);
};

// Gib chunk helper for secondary gore bursts.
void(vector org, vector dir, float count) Particles_Gib =
{
    if (count < 1)
        count = 1;

    if (particles_supported && FX_GIB >= 0)
    {
        pointparticles(FX_GIB, org, dir, count);
        return;
    }

    particle(org, dir, 0, count);
};

// Concentrated sparks for lightning gun contacts.
void(vector org, float count) Particles_LightningHit =
{
    if (count < 1)
        count = 1;

    if (particles_supported && FX_LIGHTNING >= 0)
    {
        pointparticles(FX_LIGHTNING, org, '0 0 200', count);
        return;
    }

    particle(org, '0 0 100', 225, count);
};

// Fiery burst used for barrels and small explosions.
void(vector org, float count) Particles_ExplosionBurst =
{
    if (count < 1)
        count = 1;

    if (particles_supported && FX_EXPLOSION >= 0)
    {
        pointparticles(FX_EXPLOSION, org, '0 0 0', count);
        return;
    }

    particle(org, '0 0 0', 75, count);
};
