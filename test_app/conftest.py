# SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import os

import pytest


@pytest.fixture(scope='session')
def test_app_path():
    return os.path.dirname(__file__)
