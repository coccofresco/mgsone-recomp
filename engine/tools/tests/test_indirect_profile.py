import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from indirect_profile import computed_transfer_kind  # noqa: E402


def register_jump(function, source=2, destination=31):
    return (source << 21) | (destination << 11) | function


class IndirectProfileTest(unittest.TestCase):
    def test_classifies_computed_jr_but_not_return(self):
        self.assertEqual(computed_transfer_kind(register_jump(0x08, source=2)), "jump")
        self.assertIsNone(computed_transfer_kind(register_jump(0x08, source=31)))

    def test_classifies_jalr_and_zero_link_tail_jump(self):
        self.assertEqual(computed_transfer_kind(register_jump(0x09)), "call")
        self.assertEqual(
            computed_transfer_kind(register_jump(0x09, destination=0)), "jump"
        )

    def test_rejects_non_special_instruction(self):
        self.assertIsNone(computed_transfer_kind(0x08000000))
        self.assertIsNone(computed_transfer_kind(None))


if __name__ == "__main__":
    unittest.main()
