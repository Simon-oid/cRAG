"""Tests for the crag Python binding. Standard library unittest; the NumPy
cases run only when NumPy is installed (it is optional)."""

import array
import os
import tempfile
import unittest

import crag

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None


def vec(*xs):
    return array.array("f", xs)


def fixture():
    idx = crag.Index()
    idx.add_many([
        ("alpha.md", "the quick brown fox", vec(1, 0, 0)),
        ("beta.md", "lazy dogs sleep all day", vec(0, 1, 0)),
        ("gamma.md", "a quick red fox jumps", vec(0.9, 0.1, 0)),
    ])
    return idx


class TestIndex(unittest.TestCase):
    def test_stats(self):
        with fixture() as idx:
            self.assertEqual(idx.stats(), {"chunks": 3, "sources": 3, "dim": 3})

    def test_cosine(self):
        with fixture() as idx:
            hits = idx.search(vector=vec(1, 0, 0), k=2)
            self.assertEqual([h.source for h in hits], ["alpha.md", "gamma.md"])
            self.assertAlmostEqual(hits[0].score, 1.0, places=5)

    def test_bm25(self):
        with fixture() as idx:
            hits = idx.search(text="dogs")
            self.assertEqual([h.source for h in hits], ["beta.md"])
            self.assertEqual(hits[0].cosine, -2.0)
            self.assertEqual(idx.search(text="zebra"), [])

    def test_hybrid_is_the_default_with_both(self):
        with fixture() as idx:
            hits = idx.search(vector=vec(0.9, 0.1, 0), text="red fox", k=3)
            self.assertEqual(hits[0].source, "gamma.md")
            self.assertGreater(hits[0].bm25, 0)

    def test_errors(self):
        idx = fixture()
        with self.assertRaises(crag.CragError) as cm:
            idx.add("x", "y", vec(1, 0, 0, 0))              # dim mismatch
        self.assertEqual(cm.exception.code, -6)
        with self.assertRaises(TypeError):
            idx.add("x", "y", [1.0, 0.0, 0.0])               # list: not a buffer
        with self.assertRaises(TypeError):
            idx.add("x", "y", array.array("d", [1, 0, 0]))   # float64
        with self.assertRaises(ValueError):
            idx.search(text="x", mode="nope")
        idx.close()
        with self.assertRaises(ValueError):
            idx.stats()                                      # closed

    def test_persists(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "i.db")
            with crag.Index(path) as idx:
                idx.add("disk.md", "kept on disk", vec(0, 0, 1))
            with crag.Index(path) as idx:
                self.assertEqual(idx.stats()["chunks"], 1)
                self.assertEqual(idx.search(text="disk")[0].source, "disk.md")

    def test_unicode_text(self):
        with crag.Index() as idx:
            idx.add("città.md", "perché la città è bella", vec(1, 0))
            hit = idx.search(text="città")[0]
            self.assertEqual(hit.source, "città.md")
            self.assertIn("è", hit.text)


@unittest.skipIf(np is None, "NumPy not installed")
class TestNumpy(unittest.TestCase):
    def test_float32_zero_copy(self):
        with crag.Index() as idx:
            idx.add("a", "alpha", np.array([1, 0, 0], np.float32))
            hits = idx.search(vector=np.array([1, 0, 0], np.float32))
            self.assertEqual(hits[0].source, "a")

    def test_rejects_float64_and_strided(self):
        with crag.Index() as idx:
            with self.assertRaises(TypeError):
                idx.add("a", "alpha", np.array([1, 0, 0], np.float64))
            strided = np.arange(6, dtype=np.float32)[::2]
            with self.assertRaises(TypeError):
                idx.add("a", "alpha", strided)


if __name__ == "__main__":
    unittest.main()
