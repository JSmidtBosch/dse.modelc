#! /usr/bin/python
# Copyright (C) 2021 Robert Bosch GmbH

import os
import asyncio
import pytest

pytestmark = pytest.mark.skipif(
        os.environ["PACKAGE_ARCH"] != "linux-amd64",
        reason="test only runs on linux-amd64")

TIMEOUT = 60
VALGRIND_CMD = 'valgrind --track-origins=yes --leak-check=full --error-exitcode=808'
USE_VALGRIND = True

# Sandbox for Model C
MODELC_SANDBOX_DIR = os.getenv('MODELC_SANDBOX_DIR')
SIMBUS_EXE = MODELC_SANDBOX_DIR+'/bin/simbus'
MODELC_EXE = MODELC_SANDBOX_DIR+'/bin/modelc'

# Sandbox for the Model (common only, specific in test cases).
MODEL_YAML = 'data/model.yaml'
SIGNAL_GROUP_YAML = 'data/signal_group.yaml'
STACK_YAML = 'data/stack.yaml'


async def run(dir, cmd):
    _cmd = f'cd {dir}; {VALGRIND_CMD if USE_VALGRIND else ""} {cmd}'
    print('Run:')
    print(f'  dir: {dir}')
    print(f'  cmd: {_cmd}')
    p = await asyncio.create_subprocess_shell(
        _cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    stdout, stderr = await p.communicate()
    return { 'dir': dir, 'cmd': _cmd, 'rc': p.returncode, 'stdout': stdout.decode(), 'stderr': stderr.decode() }


async def main(params, checks):
    run_list = []
    # SimBus
    run_list.append(asyncio.wait_for(run(
            params['MODEL_SANDBOX_DIR'],
            f'{SIMBUS_EXE} --logger 2 --timeout 1 {STACK_YAML}'),
        timeout=TIMEOUT)
    )
    # Models
    for m in params['models']:
        run_list.append(asyncio.wait_for(run(
            params['MODEL_SANDBOX_DIR'],
            f'{MODELC_EXE} --logger 2 ' + f'--name {m["MODEL_INST"]} ' + f'{m["MODEL_YAML_FILES"]} '), timeout = TIMEOUT))
    # Gather results.
    result_list = await asyncio.gather(*run_list)
    for result in result_list:
        print('************************************************************')
        print('************************************************************')
        print('************************************************************')
        print(f'CMD : {result["cmd"]}')
        print('++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++')
        print(f'STDOUT : \n{result["stdout"]}')
        print('++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++')
        print(f'STDERR : \n{result["stderr"]}')
        print('++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++')
        print(f'RC : {result["rc"]}')
        print('************************************************************')
        print('')
        assert result['rc'] == 0

    for check in checks:
        # Search for check in each result STDOUT
        found = False
        for result in result_list:
            if check in result['stdout']:
                found = True
        assert found, check


@pytest.mark.skip(reason="Needs refactor")
def test_modelc_stack():
    # Two models executed by the same instance of ModelC.
    params = {
        'MODEL_SANDBOX_DIR': MODELC_SANDBOX_DIR + '/examples/stacked',
        'models': [
            {
                'MODEL_INST': '"stacked_one;stacked_two"',
                'MODEL_YAML_FILES' : f'{MODEL_YAML} ' + f'{STACK_YAML} ' + f'{SIGNAL_GROUP_YAML}',
            },
        ]
    }
    checks = [
        'SignalValue: 2851307223 = 4.800000 [name=foo]',
        'SignalValue: 1991736602 = 16.800000 [name=bar]',
        'stop_time      : 0.025000',
    ]
    asyncio.run(main(params, checks))
