from pathlib import Path

import pytest

FIXTURES = Path(__file__).parent / "fixtures"


@pytest.fixture
def raw_small() -> Path:
    return FIXTURES / "raw_small"
