import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from crash3_anchored_patch_audit import verify_patch
from test_crash3_surface_partition_audit import fixture


def test_independent_source_patch_needs_geometry_material_and_both_branches():
    resource,paths=fixture()
    result=verify_patch(resource,[0,1],[2,3,4,5],paths)
    assert result['passed']
    paths[999]['masks'][(0,0,0)]=0
    paths[999]['masks'][(0,1,0)]=0
    assert not verify_patch(resource,[0,1],[2,3,4,5],paths)['passed']
    resource,paths=fixture()
    resource['source_faces'][2]['texture_extent']=[1,1]
    assert not verify_patch(resource,[0,1],[2,3,4,5],paths)['passed']


def test_source_patch_rejects_missing_faces_wrong_bindings_and_malformed_units():
    resource,paths=fixture()
    assert not verify_patch(resource,[0,1],[2,3,4],paths)['passed']
    assert not verify_patch(resource,[0],[2,3,4,5],paths)['passed']
    assert not verify_patch(resource,[0,1],[2,3,4,500],paths)['passed']
    paths[999]['resources'][0]=(42,124,6)
    assert not verify_patch(resource,[0,1],[2,3,4,5],paths)['passed']


def test_partial_native_handoff_does_not_replace_the_two_exclusive_witnesses():
    resource,paths=fixture()
    paths[999]['state_count']=3
    paths[999]['masks'][(0,0,0)]|=4
    paths[999]['masks'][(0,2,0)]|=4
    assert verify_patch(resource,[0,1],[2,3,4,5],paths)['passed']
    paths[999]['masks'][(0,2,0)]|=1
    assert not verify_patch(resource,[0,1],[2,3,4,5],paths)['passed']
