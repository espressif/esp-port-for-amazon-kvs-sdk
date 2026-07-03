# SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from pytest_embedded import Dut


def test_all(dut: Dut) -> None:
    """Run all Unity tests by sending '*' to the menu"""
    dut.expect("Press ENTER to see the list of tests")
    dut.write('')
    dut.expect("Here's the test menu")
    dut.expect("Enter test for running")
    dut.write('*')
    dut.expect(r"\d+ Tests \d+ Failures \d+ Ignored")
