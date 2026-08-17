# SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from pytest_embedded import Dut


def test_all(dut: Dut) -> None:
    """Run all Unity tests via the '*' menu entry.

    expect_unity_test_output() parses each Unity case and records it as its own
    entry in the JUnit report (so the pipeline Tests tab lists every test, not
    just one aggregate), and fails the run if any case fails.
    """
    dut.expect("Press ENTER to see the list of tests")
    dut.write('')
    dut.expect("Here's the test menu")
    dut.expect("Enter test for running")
    dut.write('*')
    dut.expect_unity_test_output()
