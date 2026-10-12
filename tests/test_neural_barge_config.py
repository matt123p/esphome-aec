"""Exercise the actual cross-field validator without the ESPHome runtime."""
import ast
import copy
from pathlib import Path
import types
import unittest
import shutil
import subprocess


class NeuralBargeConfigTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++'), 'Requires a host C++ compiler')
    def test_disabled_firmware_excludes_model_and_detector(self):
        root=Path(__file__).resolve().parents[1]
        source='#include "src/esphome/aec_speexdsp/barge_in_model.h"\n#include "src/esphome/aec_speexdsp/neural_barge_in.h"\n'
        result=subprocess.run(['g++','-DUSE_ESP32','-E','-P','-x','c++','-I',str(root),'-'],
                              input=source,text=True,capture_output=True,check=True)
        self.assertNotIn('NeuralBargeIn',result.stdout)
        self.assertNotIn('barge_model',result.stdout)

    def test_constraints(self):
        source=Path(__file__).resolve().parents[1]/'src/esphome/aec_speexdsp/__init__.py'
        tree=ast.parse(source.read_text(encoding='utf-8'))
        nodes=[n for n in tree.body if
               isinstance(n,ast.FunctionDef) and n.name in ('_validate','_resolve_reference_activity') or
               isinstance(n,ast.Assign) and isinstance(n.targets[0],ast.Name) and n.targets[0].id.startswith('CONF_')]
        namespace={'cv':types.SimpleNamespace(Invalid=ValueError)}
        exec(compile(ast.Module(body=nodes,type_ignores=[]),str(source),'exec'),namespace)
        validate=namespace['_validate']
        config=dict(vad=True,output_channel='first',beamforming={'enabled':False},
                    agc={'enabled':True,'gate':{'enabled':False,'open_rms':64,'close_rms':32}},
                    tdm_slots=4,microphone_slots=[0,1],reference_slot=2,tx_slots=[0,1],
                    reference_source='analog_slot',frame_size=256,
                    filter_length=2048,capture_only=False,
                    neural_barge_in={'enabled':True,'voice_assistant_active':'return true;'})
        validate(copy.deepcopy(config))
        for delay in (0, 7):
            hardware_delay=copy.deepcopy(config);hardware_delay['reference_delay_samples']=delay
            with self.subTest(hardware_delay=delay),self.assertRaises(ValueError):validate(hardware_delay)
        playback_delay=copy.deepcopy(config);playback_delay['reference_source']='playback'
        playback_delay['reference_delay_samples']=7
        validate(playback_delay)
        for change in [dict(frame_size=128),dict(microphone_slots=[0]),dict(capture_only=True),
                       dict(vad=False),dict(output_channel='second',microphone_slots=[0])]:
            invalid=copy.deepcopy(config);invalid.update(change)
            with self.subTest(change=change),self.assertRaises(ValueError):validate(invalid)
        validate(copy.deepcopy(config))  # Active mode needs no legacy near_end configuration.
        second=copy.deepcopy(config);second['output_channel']='second'
        validate(second)  # Either single processed microphone supports neural VAD.
        missing_activity=copy.deepcopy(config)
        del missing_activity['neural_barge_in']['voice_assistant_active']
        with self.assertRaises(ValueError):validate(missing_activity)
        disabled=copy.deepcopy(config)
        disabled['neural_barge_in']['enabled']=False
        disabled['frame_size']=128;disabled['microphone_slots']=[0]
        validate(disabled)  # Off has no neural feature-layout restrictions.
        disabled['output_channel']='second'
        with self.assertRaises(ValueError):validate(disabled)  # Second still requires two slots.
        schema_text=source.read_text(encoding='utf-8')
        self.assertNotIn('two_stage_barge_in',schema_text)
        self.assertNotIn('cv.Optional("shadow"',schema_text)


if __name__=='__main__':unittest.main()
