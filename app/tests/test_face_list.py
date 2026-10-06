"""Reading the engine's `list` answer: the three list states and what the app shows for them."""

import json
import unittest

import linux_hello_camera_common as common

parse = common.parse_face_list


def reply(**fields):
    return json.dumps(fields) + "\n"


class States(unittest.TestCase):
    def test_enrolled(self):
        out = reply(result="ok", entries=[{"id": "1759400000123", "created": 1759400000},
                                          {"id": "1759400000456", "created": 1759400001}],
                    encryption="tpm-sb", model="edgeface_s_gamma_05.onnx")
        faces = parse(out)
        self.assertEqual(faces["state"], "ok")
        self.assertEqual([e["id"] for e in faces["entries"]], ["1759400000123", "1759400000456"])
        self.assertEqual(faces["entries"][0]["created"], 1759400000)
        self.assertEqual(faces["encryption"], "tpm-sb")

    def test_tpm_only(self):
        self.assertEqual(parse(reply(result="ok", entries=[], encryption="tpm"))["encryption"], "tpm")

    def test_unencrypted(self):
        self.assertEqual(parse(reply(result="ok", entries=[], encryption="none"))["encryption"], "none")

    def test_not_enrolled(self):
        faces = parse(reply(result="not_enrolled"))
        self.assertEqual((faces["state"], faces["entries"]), ("not_enrolled", []))

    def test_needs_reenrol(self):
        faces = parse(reply(result="needs_reenrol", detail="model changed"))
        self.assertEqual(faces["state"], "needs_reenrol")
        self.assertEqual(faces["detail"], "model changed")
        self.assertEqual(faces["entries"], [])

    def test_unknown_results_and_garbage_are_errors(self):
        for text in (None, "", "garbage", "{broken", reply(result="busy"), reply(), "[1, 2]\n", "{}\n"):
            self.assertEqual(parse(text)["state"], "error", text)

    def test_the_error_detail_is_kept(self):
        self.assertEqual(parse(reply(result="error", detail="daemon down"))["detail"], "daemon down")


class Robustness(unittest.TestCase):
    def test_noise_before_the_json_line_is_skipped(self):
        self.assertEqual(parse("warning: something\n" + reply(result="not_enrolled"))["state"], "not_enrolled")

    def test_entries_with_bad_ids_are_dropped(self):
        out = reply(result="ok", entries=[{"id": "../x", "created": 1}, {"id": 5, "created": 1},
                                          {"id": "12", "created": 1}, "junk", {"created": 3}])
        self.assertEqual([e["id"] for e in parse(out)["entries"]], ["12"])

    def test_missing_or_odd_dates_become_none(self):
        out = reply(result="ok", entries=[{"id": "1"}, {"id": "2", "created": "yesterday"}, {"id": "3", "created": True}])
        self.assertEqual([e["created"] for e in parse(out)["entries"]], [None, None, None])

    def test_unknown_encryption_is_not_shown(self):
        for value in ("host", "host+tpm2", "tpm2", "aes"):
            self.assertIsNone(parse(reply(result="ok", entries=[], encryption=value))["encryption"])

    def test_embeddings_are_never_passed_on(self):
        out = reply(result="ok", entries=[{"id": "1", "created": 1, "embedding": "AAAA"}])
        self.assertEqual(set(parse(out)["entries"][0]), {"id", "created", "face"})


class Faces(unittest.TestCase):
    def test_pictures_are_grouped_by_face_with_names(self):
        out = reply(result="ok", entries=[{"id": "1", "created": 30, "face": "1"},
                                          {"id": "2", "created": 10, "face": "2"},
                                          {"id": "3", "created": 20, "face": "1"}],
                    names={"1": "Me", "2": "  ", "9": "gone"})
        faces = parse(out)
        groups = common.group_faces(faces["entries"], faces["names"])
        self.assertEqual([(g["face"], g["name"], len(g["entries"]), g["created"]) for g in groups],
                         [("1", "Me", 2, 20), ("2", "", 1, 10)])

    def test_an_old_engine_without_faces_shows_one_face(self):
        out = reply(result="ok", entries=[{"id": "1", "created": 1}, {"id": "2", "created": 2}])
        faces = parse(out)
        self.assertEqual(faces["names"], {})
        self.assertEqual(len(common.group_faces(faces["entries"], faces["names"])), 1)

    def test_bad_face_ids_and_names_are_dropped(self):
        out = reply(result="ok", entries=[{"id": "1", "face": "../x"}], names={"1": 5, "2": ["x"]})
        faces = parse(out)
        self.assertEqual(faces["entries"][0]["face"], "")
        self.assertEqual(faces["names"], {})


if __name__ == "__main__":
    unittest.main()
