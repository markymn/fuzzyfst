import pytest
import fuzzyfst

WORDS = ["apple", "banana", "cat", "car", "card", "care", "cart",
         "bat", "bar", "barn", "bare", "dark", "dart"]


@pytest.fixture
def fst_path(tmp_path):
    p = str(tmp_path / "test.fst")
    fuzzyfst.build(p, WORDS)
    return p


@pytest.fixture
def fst(fst_path):
    return fuzzyfst.Fst.open(fst_path)


def test_build_and_open(fst_path):
    fst = fuzzyfst.Fst.open(fst_path)
    assert fst.num_nodes > 0


def test_contains(fst):
    assert fst.contains("apple")
    assert fst.contains("cat")
    assert not fst.contains("notaword")


def test_fuzzy_search_basic(fst):
    results = fst.fuzzy_search("cat", 1)
    words = {w for w, d in results}
    assert "cat" in words   # distance 0
    assert "bat" in words   # distance 1
    assert "car" in words   # distance 1
    assert "cart" in words  # distance 1


def test_fuzzy_search_returns_tuples(fst):
    results = fst.fuzzy_search("cat", 1)
    for item in results:
        assert isinstance(item, tuple)
        assert isinstance(item[0], str)
        assert isinstance(item[1], int)


def test_fuzzy_search_empty_on_long_query(fst):
    results = fst.fuzzy_search("a" * 65, 1)
    assert results == []


def test_build_error_bad_path():
    with pytest.raises(ValueError):
        fuzzyfst.build("/nonexistent/dir/test.fst", ["a", "b"])


def test_open_error_bad_path():
    with pytest.raises(ValueError):
        fuzzyfst.Fst.open("/nonexistent/file.fst")


def test_string_lifetime_safety(fst):
    """Verify results are stable Python strings, not dangling views."""
    results1 = fst.fuzzy_search("cat", 1)
    results2 = fst.fuzzy_search("bar", 1)  # overwrites thread_local buffer
    # results1 should still be valid
    for word, dist in results1:
        assert isinstance(word, str)
        assert len(word) > 0
