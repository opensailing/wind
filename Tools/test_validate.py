"""Reject incomplete/contradictory Unreal evidence without launching Unreal."""
import copy
import unittest

from validate import evaluate


class HeadlessReportTests(unittest.TestCase):
    def setUp(self):
        self.names=['Studio.HeadlessUI.Fixture','Studio.Model.Fixture']
        self.report={'succeeded':2,'succeededWithWarnings':0,'failed':0,'notRun':0,'inProcess':0,
                     'tests':[{'fullTestPath':name,'state':'Success','entries':[]} for name in self.names]}

    def test_complete_report_passes(self):
        self.assertTrue(evaluate(self.report,self.names)['passed'])

    def test_missing_extra_duplicate_and_incomplete_tests_fail(self):
        for mutation in (lambda r:r['tests'].pop(),
                         lambda r:r['tests'][1].update(fullTestPath='Studio.Other'),
                         lambda r:r['tests'][1].update(fullTestPath=self.names[0]),
                         lambda r:r['tests'][1].update(state='NotRun'),
                         lambda r:r.update(inProcess=1),
                         lambda r:r.update(succeeded=1)):
            report=copy.deepcopy(self.report);mutation(report)
            self.assertFalse(evaluate(report,self.names)['passed'],report)

    def test_error_events_fail_even_with_success_exit_and_state(self):
        self.report['tests'][0]['entries']=[{'event':{'type':'Error','message':'camera changed'}}]
        result=evaluate(self.report,self.names)
        self.assertFalse(result['passed'])
        self.assertEqual(result['failures'][0]['errors'],['camera changed'])

    def test_warning_details_remain_visible(self):
        self.report['succeeded']=1;self.report['succeededWithWarnings']=1
        self.report['tests'][0]['entries']=[{'event':{'type':'Warning','message':'optional helper unavailable'}}]
        result=evaluate(self.report,self.names)
        self.assertTrue(result['passed'])
        self.assertEqual(result['warnings'][0]['messages'],['optional helper unavailable'])

    def test_empty_or_unfinished_reports_fail(self):
        self.assertFalse(evaluate({},self.names)['passed'])
        self.report['tests'][0]['state']='InProcess'
        self.assertFalse(evaluate(self.report,self.names)['passed'])


if __name__=='__main__':
    unittest.main()
