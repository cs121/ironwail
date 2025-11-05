// particles.qc – retro-minimal particle helpers for Ironwail
// Provides a compact API for mods while staying compatible with
// classic TempEntity fallbacks when effectinfo-powered particles
// are unavailable.

float FX_INVALID = -1;
float FX_SPARK = -1;
float FX_SMOKE = -1;

float particles_supported = 0;

// Optional overrides for message constants. Uncomment if your
// progs doesn't already define them in defs.qc.
// float MSG_BROADCAST = 2;
// float SVC_TEMPENTITY = 23;
// float TE_GUNSHOT = 2;

void() Particles_Init =
{
    // Detect whether the engine exposes pointparticles/effectinfo
    if (checkextension("DP_QC_POINTPARTICLES"))
    {
        particles_supported = 1;
        FX_SPARK = particleeffectnum("fx_spark_min");
        FX_SMOKE = particleeffectnum("fx_smoke_min");
    }
    else
    {
        particles_supported = 0;
        FX_SPARK = FX_INVALID;
        FX_SMOKE = FX_INVALID;
    }
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

// --- Example usage --------------------------------------------------------
void() Test_Spark =
{
    Particles_Point(FX_SPARK, self.origin, v_up, 8);
};

void() Test_SmokeBurst =
{
    Particles_Burst(FX_SMOKE, self.origin, 12);
};
