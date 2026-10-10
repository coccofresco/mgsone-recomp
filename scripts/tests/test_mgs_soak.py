import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from mgs_soak import analyze, movie_inputs, trend


class SoakTests(unittest.TestCase):
    def test_input_edges_ports_and_release_are_preserved(self):
        def row(tick,a,b):
            return f'F {tick*2} {tick} 100 2147549184 {a} 128 128 128 128 65 {b} 128 128 128 128 65'
        movie='\n'.join([row(101,65535^16,65535),row(102,65535^16,65535^8192),row(103,65535,65535^8192)])
        frames,script=movie_inputs(movie)
        self.assertEqual(frames,3)
        self.assertEqual(set(script.splitlines()),{'1 2 up 0','2 3 circle 1'})
        with self.assertRaises(ValueError):movie_inputs(movie.replace(' 102 ',' 104 '))
        with self.assertRaises(ValueError):movie_inputs(movie.replace('128 128','127 128',1))

    def test_cache_warmup_does_not_hide_a_sustained_leak(self):
        mib=1024**2
        self.assertTrue(trend([100*mib,140*mib]+[160*mib]*8,32*mib,mib)['passed'])
        self.assertFalse(trend([100*mib+i*8*mib for i in range(10)],32*mib,mib)['passed'])

    def test_missing_duplicate_and_failed_operations_never_pass(self):
        samples=[dict(state={'soak.case':0,'soak.cycle':i,'soak.rss_bytes':1000000,
                    'soak.open_fds':6,'soak.present_cpu_p95_us':-1,'soak.host_interval_p95_us':3000},
                    counters={}) for i in range(8)]
        self.assertTrue(analyze(samples,['one'],8,2)['passed'])
        self.assertFalse(analyze(samples[:-1],['one'],8,2)['passed'])
        self.assertFalse(analyze(samples+[samples[0]],['one'],8,2)['passed'])
        failed=copy.deepcopy(samples);failed[-1]['counters']['host.savestate.failed']=1
        self.assertFalse(analyze(failed,['one'],8,2)['passed'])

    def test_vulkan_run_requires_measurements_and_rejects_allocation_growth(self):
        samples=[dict(state={'soak.case':0,'soak.cycle':i,'soak.rss_bytes':1000000,
                    'soak.open_fds':6,'soak.present_cpu_p95_us':2000,'soak.host_interval_p95_us':3000},
                    counters={}) for i in range(10)]
        self.assertFalse(analyze(samples,['one'],10,2,True)['passed'])
        for row in samples:
            row['state'].update({'host.vulkan_memory.live_bytes':1000000,
                                 'host.vulkan_memory.live_allocations':8})
        self.assertTrue(analyze(samples,['one'],10,2,True)['passed'])
        for i,row in enumerate(samples):row['state']['host.vulkan_memory.live_bytes'] += i*2*1024**2
        self.assertFalse(analyze(samples,['one'],10,2,True)['passed'])
        samples[-1]['counters']['host.vulkan_memory.untracked_free']=1
        self.assertIn('one: Vulkan allocation error',analyze(samples,['one'],10,2,True)['failures'])

    def test_fd_growth_and_presentation_degradation_are_independent(self):
        samples=[dict(state={'soak.case':0,'soak.cycle':i,'soak.rss_bytes':1000000,
                    'soak.open_fds':6+i,'soak.present_cpu_p95_us':2000+i*700,'soak.host_interval_p95_us':3000},
                    counters={}) for i in range(10)]
        result=analyze(samples,['one'],10,2)
        self.assertFalse(result['passed'])
        self.assertEqual(len(result['failures']),2)

    def test_unavailable_wine_handle_count_is_not_reported_as_leak_free(self):
        samples=[dict(state={'soak.case':0,'soak.cycle':i,'soak.rss_bytes':1000000,
                    'soak.open_handles':0,'soak.present_cpu_p95_us':2000,'soak.host_interval_p95_us':3000},
                    counters={}) for i in range(8)]
        result=analyze(samples,['one'],8,2)
        self.assertTrue(result['passed'])
        self.assertFalse(result['cases'][0]['windows_handles']['available'])
        self.assertIsNone(result['cases'][0]['windows_handles']['passed'])

if __name__=='__main__':unittest.main()
