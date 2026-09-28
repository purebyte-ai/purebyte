// The path rules of the profiles (no model needed): which paths the secrets-code profile takes for tests, examples or
// documentation, whose findings are warnings (detect/code_files.h), and which profiles apply them.
#include <string>

#include "detect/code_files.h"
#include "detect/profile.h"
#include "test.h"

using pb::detect::is_test_example_or_doc_path;
using pb::detect::Severity;

TEST(paths_test_example_and_documentation_folders) {
    for (const char* path : {"tests/app.py",
                             "test/app.py",
                             "src/test/java/com/acme/KeyStore.java",
                             "src/test/resources/application.yml",
                             "app/src/jvmTest/kotlin/Keys.kt",
                             "app/src/androidTest/java/Keys.java",
                             "web/__tests__/api.js",
                             "web/__mocks__/client.js",
                             "pkg/testdata/server.pem",
                             "test-data/users.json",
                             "spec/models/user.rb",
                             "specs/config.yml",
                             "fixtures/key.pem",
                             "fixture/data.json",
                             "docs/setup.txt",
                             "doc/config.ini",
                             "examples/demo.py",
                             "example/app.js",
                             "samples/config.yaml",
                             "sample/settings.env",
                             "mocks/server.go",
                             "mock/api.py",
                             "e2e/login.ts",
                             "cypress/support/commands.js",
                             "./tests/app.py",
                             "project/tests/deep/a/b.cfg",
                             "archive.zip!/tests/key.pem",
                             "tests/fixtures.zip!/app.py"})
        CHECK_MSG(is_test_example_or_doc_path(path), path);
}

TEST(paths_test_and_documentation_files) {
    for (const char* path :
         {"handler_test.go",  "pkg/handler_test.go", "test_api.py",        "src/app.test.js",  "src/app.spec.ts",
          "lib/user_spec.rb", "util-tests.js",       "store_test.rs",      "client_test.rb",   "UserTest.java",
          "Api/UserTests.cs", "KeysTest.kt",         "LoginTests.swift",   "ConfigTest.scala", "test.java",
          "README.md",        "guide/setup.rst",     "CHANGELOG.markdown", "site/page.mdx",    "manual.adoc",
          "book.asciidoc"})
        CHECK_MSG(is_test_example_or_doc_path(path), path);
}

TEST(paths_windows_separators_and_any_case) {
    for (const char* path :
         {"C:\\work\\repo\\tests\\app.py", "src\\test\\java\\Keys.java", "repo\\docs\\setup.txt",
          "pkg\\handler_test.go", ".\\examples\\demo.py", "C:/work/repo/src/jvmTest/Keys.kt", "Tests/App.py",
          "DOCS/Setup.txt", "Examples/x.py", "SRC/TEST/Keys.java", "JVMTEST/Keys.kt", "__TESTS__/api.js", "README.MD",
          "Guide.RST", "HANDLER_TEST.GO", "Test_Api.PY", "App.Test.JS", "UserTest.JAVA", "Spec\\Models\\User.rb"})
        CHECK_MSG(is_test_example_or_doc_path(path), path);
}

TEST(paths_production_code_and_near_misses) {
    for (const char* path : {"",
                             "src/app.py",
                             "config/settings.yml",
                             ".env",
                             "src/main/java/com/acme/KeyStore.java",
                             "deploy/prod.tf",
                             "latest.java",
                             "src/Latest.java",
                             "Contest.kt",
                             "attest.py",
                             "unittest.py",
                             "tests.py",
                             "spec.json",
                             "testing/app.py",
                             "contest/app.py",
                             "mytest/x.py",
                             "test-app/x.py",
                             "src/testutils/keys.go",
                             "api/attestation.go",
                             "latest_version.go",
                             "docs.py",
                             "notes.txt",
                             "index.html",
                             "Makefile",
                             "e2e.py",
                             "app/sampler.py",
                             "src/specification.py",
                             "C:\\Users\\me\\Documents\\app\\main.py",
                             "/home/me/Documents/x.go",
                             "archive.zip!/src/app.py"})
        CHECK_MSG(!is_test_example_or_doc_path(path), path);
}

TEST(paths_only_secrets_code_lowers_severity) {
    CHECK(pb::detect::secrets_code_profile().path_severity("tests/app.py") == Severity::warning);
    CHECK(pb::detect::secrets_code_profile().path_severity("docs/README.md") == Severity::warning);
    CHECK(pb::detect::secrets_code_profile().path_severity("src/app.py") == Severity::error);
    // Binaries ship wherever they sit, and a redacted copy must hide every value: no path rules there.
    for (const pb::detect::Profile* p :
         {&pb::detect::secrets_binary_profile(), &pb::detect::redact_profile(), &pb::detect::none_profile()}) {
        CHECK_MSG(p->path_severity("tests/fixtures/app.jar") == Severity::error, p->name());
        CHECK_MSG(p->path_severity("docs/README.md") == Severity::error, p->name());
    }
    CHECK(std::string(pb::detect::severity_name(Severity::error)) == "error");
    CHECK(std::string(pb::detect::severity_name(Severity::warning)) == "warning");
}
