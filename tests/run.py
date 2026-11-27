import unittest


class ReadableTestResult(unittest.TextTestResult):
    def getDescription(self, test):
        module = type(test).__module__.rsplit(".", 1)[-1]
        description = test.shortDescription()
        if self.descriptions and description and module.startswith("test_"):
            return f"[{module[5:]}] {description}"
        # unittest's subtest descriptions inherit the parent test's docstring.
        return str(test)


class ReadableTestRunner(unittest.TextTestRunner):
    resultclass = ReadableTestResult


if __name__ == "__main__":
    unittest.main(module=None, testRunner=ReadableTestRunner)
