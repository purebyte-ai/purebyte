// Which files the `secrets-code` profile reads while walking a tree: source code, configuration and text files by
// name; dependency, virtual-environment and build folders are skipped. A file named explicitly is always read. And
// which paths hold tests, examples or documentation: the profile's findings there are warnings, not errors.
#pragma once

#include <cstdint>
#include <string>

namespace pb::detect {

// By extension (after the last '.', case-insensitive) or by whole name (Dockerfile, .env, credentials...).
bool is_source_text_name(const std::string& file_name);
// .git, node_modules, __pycache__, .venv, venv, dist, build, .next, target, vendor.
bool is_dependency_directory(const std::string& directory_name);

// Test, example and documentation paths, where most findings are credentials made for tests and examples: the
// profile reports them as warnings instead of errors. `path` is a file's path inside the project scanned, with `/` or
// `\` as separators. True when any part of it is (in any case) test, tests, spec, specs, __tests__, testdata,
// test-data, fixture, fixtures, jvmTest, androidTest, example, examples, sample, samples, doc, docs, mock, mocks,
// __mocks__, e2e or cypress; when the file is named as a test (app.test.js, user_spec.rb, handler_test.go,
// test_api.py, UserTest.java, UserTests.cs); or when it is a document (.md, .markdown, .mdx, .rst, .adoc, .asciidoc).
bool is_test_example_or_doc_path(const std::string& path);

// The largest text file the profile analyzes.
constexpr uint64_t kTextMaxBytes = 4000000;

}  // namespace pb::detect
