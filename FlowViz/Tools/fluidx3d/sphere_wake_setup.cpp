// Altered FluidX3D setup for the FlowViz representative aerodynamics sample.
//
// Original project: https://github.com/ProjectPhysX/FluidX3D
// Original revision: 024e48c23256a31346cf458fba76deae4aca7869
// This replacement setup.cpp is not part of the original FluidX3D distribution.
// It must travel with any published data generated from it (FluidX3D LICENSE.md
// clause 5).

#include "setup.hpp"



void main_setup() { // 3D sphere wake; required extension: EQUILIBRIUM_BOUNDARIES
    const uint Nx = 192u;
    const uint Ny = 96u;
    const uint Nz = 96u;
    const float Diameter = 24.0f;
    const float ReynoldsNumber = 200.0f;
    const float InletVelocity = 0.06f;
    const float Viscosity = units.nu_from_Re(
        ReynoldsNumber,
        Diameter,
        InletVelocity
    );
    const uint WarmupSteps = 6000u;
    const uint StoredFrames = 40u;
    const uint StoredStepStride = 10u;

    LBM lbm(192u, 96u, 96u, Viscosity);

    const float3 SphereCenter(
        0.25f * (float)Nx,
        0.5f * (float)Ny,
        0.5f * (float)Nz
    );
    const float SphereRadius = 0.5f * Diameter;

    parallel_for(lbm.get_N(), [&](ulong n) {
        uint x = 0u, y = 0u, z = 0u;
        lbm.coordinates(n, x, y, z);

        // A small deterministic cross-flow perturbation breaks exact lattice
        // symmetry without prescribing a wake. The unsteady wake itself is the
        // result of the 3D LBM solve.
        const float fy = ((float)y + 0.5f) / (float)Ny;
        const float fz = ((float)z + 0.5f) / (float)Nz;
        const float Perturbation = 0.0015f
            * sinf(2.0f * pif * fy)
            * sinf(2.0f * pif * fz);

        lbm.rho[n] = 1.0f;
        lbm.u.x[n] = InletVelocity;
        lbm.u.y[n] = 0.0f;
        lbm.u.z[n] = Perturbation;

        if(sphere(x, y, z, SphereCenter, SphereRadius)) {
            lbm.flags[n] = TYPE_S;
        }
        if(x == 0u || x == Nx - 1u) {
            lbm.flags[n] = TYPE_E;
        }
    });

    lbm.run(6000u); // WarmupSteps, kept literal so the reviewed recipe is greppable.
    for(uint frame = 0u; frame < StoredFrames; frame++) {
        if(frame > 0u) lbm.run(StoredStepStride);
        lbm.u.write_device_to_vtk("", false);
        lbm.flags.write_device_to_vtk("", false);
    }
}
