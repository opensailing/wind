"""Structural numerical tests, explicitly not published CFD acceptance."""
import unittest
import numpy as np
from scipy.spatial import Delaunay
from reconstruct_cylinder_volume import build_mapping,cylinder_clearance,QualifiedLocator


class VolumeReconstructionTests(unittest.TestCase):
    def test_locator_requires_containment_despite_vtk_parametric_tolerance(self):
        xyz=np.array([[0,0,0],[1,0,0],[0,1,0],[0,0,1],[0,0,-1]],dtype=float)
        tri=Delaunay(xyz)
        locator=QualifiedLocator(tri,np.ones(len(tri.simplices),bool))
        positions=np.array([[.2,.2,1e-5],[.2,.2,-1e-5],[-1e-5,.2,.2],[.2,.2,0.]])
        result=locator.find(positions)
        self.assertTrue(np.all(result[[0,1,3]]>=0))
        self.assertEqual(result[2],-1,'No outside extrapolation through VTK tolerance')
        for index in [0,1,3]:
            transform=tri.transform[result[index]]
            weights=transform[:3]@(positions[index]-transform[3])
            weights=np.r_[weights,1-weights.sum()]
            self.assertTrue(np.all(weights>=0))
            np.testing.assert_allclose(weights@xyz[tri.simplices[result[index]]],positions[index],atol=1e-16)

    def test_projected_tetrahedron_solid_intersection(self):
        outside=[[-2,2,0],[-1,2,0],[-1,3,0],[-1,2,1]]
        crossing=[[-1,-1,0],[1,-1,0],[0,1,0],[0,0,1]]
        tangent=[[1,0,0],[2,-1,0],[2,1,0],[1,0,1]]
        degenerate=[[2,0,0],[2,0,1],[3,0,0],[3,0,1]]
        np.testing.assert_allclose(cylinder_clearance(np.array([outside,crossing,tangent,degenerate])),
                                   [np.sqrt(5),0,1,2],rtol=1e-14)

    def test_stencils_preserve_affine_fields_and_mask_the_solid(self):
        x=np.linspace(-2,2,9);z=np.linspace(0,1,3)
        xyz=np.array([(a,b,c) for c in z for b in x for a in x if a*a+b*b>=.75*.75]+[(0,0,.5)])
        dims=np.array([17,17,5])
        rows,weights,classes,lo,hi,report=build_mapping(xyz,dims,.75,1.5)
        indexes=np.flatnonzero(classes==1)
        self.assertGreater(len(indexes),200)
        self.assertGreater(report['solid_nodes'],0)
        self.assertEqual(report['original_rows_inside_analytic_solid_excluded_from_interpolation'],1)
        self.assertFalse(np.any(rows[classes==1]==len(xyz)-1))
        grid=np.column_stack((indexes%dims[0],indexes//dims[0]%dims[1],indexes//(dims[0]*dims[1])))
        positions=lo+grid/(dims-1)*(hi-lo)
        values=2*xyz[:,0]-3*xyz[:,1]+7*xyz[:,2]+11
        actual=np.sum(values[rows[indexes]]*weights[indexes],axis=1)
        expected=2*positions[:,0]-3*positions[:,1]+7*positions[:,2]+11
        np.testing.assert_allclose(actual,expected,rtol=1e-13,atol=1e-13)
        self.assertTrue(np.all(np.linalg.norm(positions[:,:2],axis=1)>=.75))
        self.assertTrue(np.all(rows[classes!=1]==np.iinfo(np.uint32).max))
        self.assertTrue(np.all(weights[classes!=1]==0))
        self.assertLess(report['maximum_coordinate_reproduction_error_m'],1e-12)

    def test_invalid_geometry_and_budgets(self):
        xyz=np.array([[0,0,0],[1,0,0],[0,1,0],[0,0,1]],dtype=float)
        with self.assertRaisesRegex(ValueError,'supported fluid'):
            build_mapping(xyz,[3,3,3],.5,3)
        with self.assertRaisesRegex(ValueError,'native limits'):
            build_mapping(xyz,[512,512,512],.5,3)


if __name__=='__main__':unittest.main()
