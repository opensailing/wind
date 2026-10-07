// A view-facing proxy covers the viewport; slab and depth tests bound every ray.
float3 direction = Orthographic > .5 ? normalize(RenderCameraForward) : normalize(WorldPosition - RenderCameraPosition);
float3 origin = RenderCameraPosition;
if (Orthographic > .5)
    origin = WorldPosition - direction * dot(WorldPosition - RenderCameraPosition, direction);
float3 lower = VolumeMinimum + ClipMinimum * VolumeSize;
float3 upper = VolumeMinimum + ClipMaximum * VolumeSize;
float entry = 0;
float leave = 1e20;
[unroll] for (int axis = 0; axis < 3; ++axis)
{
    if (abs(direction[axis]) < 1e-8)
    {
        if (origin[axis] < lower[axis] || origin[axis] > upper[axis]) return float4(0,0,0,0);
    }
    else
    {
        float a = (lower[axis] - origin[axis]) / direction[axis];
        float b = (upper[axis] - origin[axis]) / direction[axis];
        entry = max(entry, min(a,b));
        leave = min(leave, max(a,b));
    }
}
float forwardCosine = max(dot(direction, normalize(RenderCameraForward)), 1e-6);
entry = max(entry, CameraNearDepth / forwardCosine);
leave = min(leave, min(OpaqueDepth, CameraFarDepth) / forwardCosine);
if (leave <= entry || Opacity <= 0) return float4(0,0,0,0);
float3 sceneDimensions = GridDimensions.xzy;
float3 spacing = VolumeSize / (sceneDimensions - 1);
float referenceStep = min(spacing.x, min(spacing.y, spacing.z));
float desiredStep = max(referenceStep * StepVoxels, 1e-7);
int count = clamp((int)ceil((leave - entry) / desiredStep), 1, 2048);
float stepSize = (leave - entry) / count;
float4 accumulated = float4(0,0,0,0);
[loop] for (int i = 0; i < count; ++i)
{
    float3 position = origin + direction * (entry + (i + .5) * stepSize);
    float3 sourceUVW = ((position - VolumeMinimum) / VolumeSize).xzy;
    float3 node = saturate(sourceUVW) * (GridDimensions - 1);
    int3 cell = min((int3)floor(node), (int3)GridDimensions - 2);
    // Discard any interpolation cell touching the explicit cylindrical solid.
    // This matches CPU queries and the isosurface support rule.
    float3 cellMinimum = SourceMinimum + (float3)cell * SourceSize / (GridDimensions - 1);
    float3 cellMaximum = cellMinimum + SourceSize / (GridDimensions - 1);
    float2 closest = clamp(Cylinder.xy, cellMinimum.xy, cellMaximum.xy) - Cylinder.xy;
    if (dot(closest,closest) < Cylinder.z*Cylinder.z) continue;
    float3 fraction = node - cell;
    float scalar = 0;
    float extinction = 0;
    bool valid = true;
    [unroll] for (int corner=0;corner<8;++corner)
    {
        int3 offset=int3(corner&1,(corner>>1)&1,(corner>>2)&1);
        float2 value=VolumeScalars.Load(int4(cell+offset,0)).rg;
        valid = valid && value.g > .5;
        float3 w=lerp(1-fraction,fraction,(float3)offset);
        scalar += value.r*w.x*w.y*w.z;
        if (IndependentOpacity > .5) extinction += OpacityScalars.Load(int4(cell+offset,0)).r*w.x*w.y*w.z;
    }
    if (!valid) continue;
    if (ThresholdEnabled > .5 && (scalar < ThresholdMinimum || scalar > ThresholdMaximum)) continue;
    float t = saturate(scalar);
    float3 color;
    if (Palette > 2.5) color = t < .5 ? lerp(LowColor,MiddleColor,t*2) : lerp(MiddleColor,HighColor,(t-.5)*2);
    else if (Palette > 1.5) color = t.xxx;
    else if (Palette > .5)
        color = t < .5 ? lerp(float3(.03,.15,.6),float3(.94,.94,.94),t*2) : lerp(float3(.94,.94,.94),float3(.7,.035,.025),(t-.5)*2);
    else
    {
        float3 c0=float3(.025,.02,.35),c1=float3(.015,.14,.9),c2=float3(0,.7,.95),
            c3=float3(.02,.65,.25),c4=float3(.95,.85,.015),c5=float3(1,.25,.015),c6=float3(.8,.015,.008);
        float q=t*6;
        color=q<1?lerp(c0,c1,q):q<2?lerp(c1,c2,q-1):q<3?lerp(c2,c3,q-2):
            q<4?lerp(c3,c4,q-3):q<5?lerp(c4,c5,q-4):lerp(c5,c6,q-5);
    }
    float curve = t < .5 ? lerp(OpacityCurve.x,OpacityCurve.y,t*2) : lerp(OpacityCurve.y,OpacityCurve.z,(t-.5)*2);
    if (IndependentOpacity > .5) curve = max(extinction,0);
    // Opacity is extinction over 32 reference voxels; use physical path length
    // so changing sampling quality does not change the optical thickness.
    float alpha = 1 - exp(-max(curve,0) * Opacity * stepSize / (referenceStep * 32));
    accumulated.rgb += (1-accumulated.a) * alpha * color;
    accumulated.a += (1-accumulated.a) * alpha;
    if (accumulated.a > .995) break;
}
return float4(accumulated.rgb / max(accumulated.a,1e-8), accumulated.a);
