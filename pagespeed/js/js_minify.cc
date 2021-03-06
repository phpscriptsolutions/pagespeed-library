// Copyright 2010 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "pagespeed/js/js_minify.h"
#include "pagespeed/js/js_keywords.h"

#include <string>

#include "base/logging.h"
#include "base/string_piece.h"

using pagespeed::JsKeywords;

namespace {

// Javascript's grammar has the appalling property that it cannot be lexed
// without also being parsed, due to its semicolon insertion rules and the
// ambiguity between regex literals and the division operator.  We don't want
// to build a full parser just for the sake of removing whitespace/comments, so
// this code uses some heuristics to try to guess the relevant parsing details.

const int kEOF = -1;  // represents the end of the input

// A token can either be a character (0-255) or one of these constants:
const int kStartToken = 256;  // the start of the input
const int kCCCommentToken = 257;  // a conditional compilation comment
const int kRegexToken = 258;  // a regular expression literal
const int kStringToken = 259;  // a string literal
// We have to differentiate between the return/throw keywords and all other
// names/keywords, to ensure that we don't treat return or throw as a primary
// expression (which could mess up linebreak removal or differentiating between
// division and regexes).
const int kNameNumberToken = 260; // name, number, or keyword other than return
const int kKeywordCanPrecedeRegExToken = 261;
// The ++ and -- tokens affect the semicolon insertion rules in Javascript, so
// we need to track them carefully in order to get whitespace removal right.
// Other multicharacter operators (such as += or ===) can just be treated as
// multiple single character operators, and it'll all come out okay.
const int kPlusPlusToken = 262;  // a ++ token
const int kMinusMinusToken = 263;  // a -- token

// Is this a character that can appear in identifiers?
int IsIdentifierChar(int c) {
  // Note that backslashes can appear in identifiers due to unicode escape
  // sequences (e.g. \u03c0).
  return ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c == '\\' ||
          c >= 127);
}

// Return true if the given token cannot ever be the first or last token of a
// statement; that is, a semicolon will never be inserted next to this token.
// This function is used to help us with linebreak suppression.
bool CannotBeginOrEndStatement(int token) {
  switch (token) {
    case kStartToken:
    case '=':
    case '<':
    case '>':
    case ';':
    case ':':
    case '?':
    case '|':
    case '^':
    case '&':
    case '*':
    case '/':
    case '%':
    case ',':
    case '.':
      return true;
    default:
      return false;
  }
}

// Return true if the given token signifies that we are at the end of a primary
// expression (e.g. 42, or foo[0], or func()).  This function is used to help
// us with linebreak suppression and to tell the difference between regex
// literals and division operators.
bool EndsPrimaryExpression(int token) {
  switch (token) {
    case kNameNumberToken:
    case kRegexToken:
    case kStringToken:
    case ')':
    case ']':
      return true;
    default:
      return false;
  }
}

// Return true if we can safely remove a linebreak from between the given two
// tokens (that is, if we're sure that the linebreak will not result in
// semicolon insertion), or false if we're not sure we can remove it safely.
bool CanSuppressLinebreak(int prev_token, int next_token) {
  // We can suppress the linebreak if the previous token can't possibly be
  // the end of a statement.
  if (CannotBeginOrEndStatement(prev_token) ||
      prev_token == '(' || prev_token == '[' || prev_token == '{' ||
      prev_token == '!' || prev_token == '~' ||
      prev_token == '+' || prev_token == '-') {
    return true;
  }
  // We can suppress the linebreak if the next token can't possibly be the
  // beginning of a statement.
  if (CannotBeginOrEndStatement(next_token) ||
      next_token == ')' || next_token == ']' ||
      next_token == '}') {
    return true;
  }
  // We can suppress the linebreak if one-token lookahead tells us that we
  // could keep parsing without inserting a semicolon.
  if (EndsPrimaryExpression(prev_token) &&
      (next_token == '(' || next_token == '[' ||
       next_token == '+' || next_token == '-')) {
    return true;
  }
  // Otherwise, we should leave the linebreak there, to be safe.
  return false;
}

class StringConsumer {
 public:
  explicit StringConsumer(std::string* output) : output_(output) {}
  void push_back(char character) {
    output_->push_back(character);
  }
  void append(const base::StringPiece& str) {
    output_->append(str.data(), str.size());
  }
  std::string* output_;
};

class SizeConsumer {
 public:
  explicit SizeConsumer(std::string* ignored) : size_(0) {}
  void push_back(char character) {
    ++size_;
  }
  void append(const base::StringPiece& str) {
    size_ += str.size();
  }
  int size_;
};

template<typename OutputConsumer>
class Minifier {
 public:
  Minifier(const base::StringPiece& input, std::string* output);
  ~Minifier() {}

  // Return a pointer to an OutputConsumer instance if minification was
  // successful, NULL otherwise.
  OutputConsumer* GetOutput();

  // Enable collapsing strings while minifiying. Should call after constructor,
  // and before calling GetOutput().
  void EnableStringCollapse() { collapse_string_ = true; }

 private:
  int Peek();
  void ChangeToken(int next_token);
  void InsertSpaceIfNeeded();
  void ConsumeBlockComment();
  void ConsumeLineComment();
  void ConsumeNameOrNumber();
  void ConsumeRegex();
  void ConsumeString();
  void Minify();

  // Represents what kind of whitespace we've seen since the last token:
  //   NO_WHITESPACE means that there is no whitespace between the tokens.
  //   SPACE means there's been at least one space/tab, but no linebreaks.
  //   LINEBREAK means there's been at least one linebreak.
  enum Whitespace { NO_WHITESPACE, SPACE, LINEBREAK };

  const base::StringPiece input_;
  int index_;
  OutputConsumer output_;
  Whitespace whitespace_;  // whitespace since the previous token
  int prev_token_;
  bool error_;
  bool collapse_string_;
};

template<typename OutputConsumer>
Minifier<OutputConsumer>::Minifier(const base::StringPiece& input,
                                   std::string* output)
  : input_(input),
    index_(0),
    output_(output),
    whitespace_(NO_WHITESPACE),
    prev_token_(kStartToken),
    error_(false),
    collapse_string_(false) {}

// Return the next character after index_, or kEOF if there aren't any more.
template<typename OutputConsumer>
int Minifier<OutputConsumer>::Peek() {
  return (index_ + 1 < input_.size() ?
          static_cast<int>(input_[index_ + 1]) : kEOF);
}

// Switch to a new prev_token, and insert a newline if necessary.  Call this
// right before appending a token onto the output.
template<typename OutputConsumer>
void Minifier<OutputConsumer>::ChangeToken(int next_token) {
  // If there've been any linebreaks since the previous token, we may need to
  // insert a linebreak here to avoid running afoul of semicolon insertion
  // (that is, the code may be relying on semicolon insertion here, and
  // removing the linebreak would break it).
  if (whitespace_ == LINEBREAK &&
      !CanSuppressLinebreak(prev_token_, next_token)) {
    output_.push_back('\n');
  }
  whitespace_ = NO_WHITESPACE;
  prev_token_ = next_token;
}

// If there's been any whitespace since the previous token, insert some
// whitespace now to separate the previous token from the next token.
template<typename OutputConsumer>
void Minifier<OutputConsumer>::InsertSpaceIfNeeded() {
  switch (whitespace_) {
    case SPACE:
      output_.push_back(' ');
      break;
    case LINEBREAK:
      output_.push_back('\n');
      break;
    default:
      break;
  }
  whitespace_ = NO_WHITESPACE;
}

template<typename OutputConsumer>
void Minifier<OutputConsumer>::ConsumeBlockComment() {
  DCHECK(index_ + 1 < input_.size());
  DCHECK(input_[index_] == '/');
  DCHECK(input_[index_ + 1] == '*');
  const int begin = index_;
  index_ += 2;
  // We want to remove comments, but we need to preserve IE conditional
  // compilation comments to avoid breaking scripts that rely on them.
  // See http://code.google.com/p/page-speed/issues/detail?id=198
  const bool may_be_ccc = (index_ < input_.size() && input_[index_] == '@');
  while (index_ < input_.size()) {
    if (input_[index_] == '*' && Peek() == '/') {
      index_ += 2;
      if (may_be_ccc && input_[index_ - 3] == '@') {
        ChangeToken(kCCCommentToken);
        output_.append(input_.substr(begin, index_ - begin));
      } else if (whitespace_ == NO_WHITESPACE) {
        whitespace_ = SPACE;
      }
      return;
    }
    ++index_;
  }
  // If we reached EOF without the comment being closed, then this is an error.
  error_ = true;
}

template<typename OutputConsumer>
void Minifier<OutputConsumer>::ConsumeLineComment() {
  while (index_ < input_.size() && input_[index_] != '\n' &&
         input_[index_] != '\r') {
    ++index_;
  }
  whitespace_ = LINEBREAK;
}

// Consume a keyword, name, or number.
template<typename OutputConsumer>
void Minifier<OutputConsumer>::ConsumeNameOrNumber() {
  if (prev_token_ == kNameNumberToken ||
      prev_token_ == kKeywordCanPrecedeRegExToken ||
      prev_token_ == kRegexToken) {
    InsertSpaceIfNeeded();
  }
  std::string token;
  while (index_ < input_.size() && IsIdentifierChar(input_[index_])) {
    token.push_back(input_[index_]);
    ++index_;
  }
  // For the most part, we can just treat keywords the same as identifiers, and
  // we'll still minify correctly. However, some keywords (like return and
  // throw) in particular must be treated differently, to help us tell the
  // difference between regex literals and division operators:
  //   return/ x /g;  // this returns a regex literal; preserve whitespace
  //   reTurn/ x /g;  // this performs two divisions; remove whitespace
  ChangeToken(JsKeywords::CanKeywordPrecedeRegEx(token)
                ? kKeywordCanPrecedeRegExToken
                : kNameNumberToken);
  output_.append(token);
}

template<typename OutputConsumer>
void Minifier<OutputConsumer>::ConsumeRegex() {
  DCHECK(index_ < input_.size());
  DCHECK(input_[index_] == '/');
  const int begin = index_;
  ++index_;
  bool within_brackets = false;
  while (index_ < input_.size()) {
    const char ch = input_[index_];
    ++index_;
    if (ch == '\\') {
      // If we see a backslash, don't check the next character (this is mainly
      // relevant if the next character is a slash that would otherwise close
      // the regex literal, or a closing bracket when we are within brackets).
      ++index_;
    } else if (ch == '/') {
      // Slashes within brackets are implicitly escaped.
      if (!within_brackets) {
        // Don't accidentally create a line comment.
        if (prev_token_ == '/') {
          InsertSpaceIfNeeded();
        }
        ChangeToken(kRegexToken);
        output_.append(input_.substr(begin, index_ - begin));
        return;
      }
    } else if (ch == '[') {
      // Regex brackets don't nest, so we don't need a stack -- just a bool.
      within_brackets = true;
    } else if (ch == ']') {
      within_brackets = false;
    } else if (ch == '\n') {
      break;  // error
    }
  }
  // If we reached newline or EOF without the regex being closed, then this is
  // an error.
  error_ = true;
}

template<typename OutputConsumer>
void Minifier<OutputConsumer>::ConsumeString() {
  DCHECK(index_ < input_.size());
  const int begin = index_;
  const char quote = input_[begin];
  DCHECK(quote == '"' || quote == '\'' || quote == '`');
  ++index_;
  while (index_ < input_.size()) {
    const char ch = input_[index_];
    ++index_;
    if (ch == '\\') {
      ++index_;
    } else {
      if (ch == quote) {
        ChangeToken(kStringToken);
        if (collapse_string_) {
          output_.push_back(quote);
          output_.push_back(quote);
        } else {
          output_.append(input_.substr(begin, index_ - begin));
        }
        return;
      }
    }
  }
  // If we reached EOF without the string being closed, then this is an error.
  error_ = true;
}

template<typename OutputConsumer>
void Minifier<OutputConsumer>::Minify() {
  while (index_ < input_.size() && !error_) {
    const char ch = input_[index_];
    // Track whitespace since the previous token.  NO_WHITESPACE means no
    // whitespace; LINEBREAK means there's been at least one linebreak; SPACE
    // means there's been spaces/tabs, but no linebreaks.
    if (ch == '\n' || ch == '\r') {
      whitespace_ = LINEBREAK;
      ++index_;
    }
    else if (ch == ' ' || ch == '\t') {
      if (whitespace_ == NO_WHITESPACE) {
        whitespace_ = SPACE;
      }
      ++index_;
    }
    // Strings:
    else if (ch == '\'' || ch == '"' || ch == '`') {
      ConsumeString();
    }
    // A slash could herald a line comment, a block comment, a regex literal,
    // or a mere division operator; we need to figure out which it is.
    // Differentiating between division and regexes is mostly impossible
    // without parsing, so we do our best based on the previous token.
    else if (ch == '/') {
      const int next = Peek();
      if (next == '/') {
        ConsumeLineComment();
      } else if (next == '*') {
        ConsumeBlockComment();
      }
      // If the slash is following a primary expression (like a literal, or
      // (...), or foo[0]), then it's definitely a division operator.  These
      // are previous tokens for which (I think) we can be sure that we're
      // following a primary expression.
      else if (EndsPrimaryExpression(prev_token_)) {
        ChangeToken('/');
        output_.push_back(ch);
        ++index_;
      } else {
        // If we can't be sure it's division, then we must assume it's a regex
        // so that we don't remove whitespace that we shouldn't.  There are
        // cases that we'll get wrong, but it's hard to do better without
        // parsing.
        ConsumeRegex();
      }
    }
    // Identifiers, keywords, and numeric literals:
    else if (IsIdentifierChar(ch)) {
      ConsumeNameOrNumber();
    }
    // Treat <!-- as a line comment.  Note that the substr() here is very
    // efficient because input_ is a StringPiece, not a std::string.
    else if (ch == '<' && input_.substr(index_).starts_with("<!--")) {
      ConsumeLineComment();
    }
    // Treat --> as a line comment if it's at the start of a line.
    else if (ch == '-' &&
             (whitespace_ == LINEBREAK || prev_token_ == kStartToken) &&
             input_.substr(index_).starts_with("-->")) {
      ConsumeLineComment();
    }
    // Treat ++ differently than two +'s.  It has different whitespace rules:
    //   - A statement cannot ever end with +, but it can end with ++.  Thus, a
    //     linebreak after + can always be removed (no semicolon will be
    //     inserted), but a linebreak after ++ generally cannot.
    //   - A + at the start of a line can continue the previous line, but a ++
    //     cannot (a linebreak is _not_ permitted between i and ++ in an i++
    //     statement).  Thus, a linebreak just before a + can be removed in
    //     certain cases (if we can decide that a semicolon would not be
    //     inserted), but a linebreak just before a ++ never can.
    else if (ch == '+' && Peek() == '+') {
      // Careful to leave whitespace so as not to create a +++ or ++++, which
      // can be ambiguous.
      if (prev_token_ == '+' || prev_token_ == kPlusPlusToken) {
        InsertSpaceIfNeeded();
      }
      ChangeToken(kPlusPlusToken);
      output_.append("++");
      index_ += 2;
    }
    // Treat -- differently than two -'s.  It has different whitespace rules,
    // analogous to those of ++ (see above).
    else if (ch == '-' && Peek() == '-') {
      // Careful to leave whitespace so as not to create a --- or ----, which
      // can be ambiguous.  Also careful of !'s, since we don't want to
      // accidentally create an SGML line comment.
      if (prev_token_ == '-' || prev_token_ == kMinusMinusToken ||
          prev_token_ == '!') {
        InsertSpaceIfNeeded();
      }
      ChangeToken(kMinusMinusToken);
      output_.append("--");
      index_ += 2;
    } else {
      // Copy other characters over verbatim, but make sure not to join two +
      // tokens into ++ or two - tokens into --, or to join ++ and + into +++
      // or -- and - into ---, or to minify the sequence of tokens < ! - - into
      // an SGML line comment.
      if ((prev_token_ == ch && (ch == '+' || ch == '-')) ||
          (prev_token_ == kPlusPlusToken && ch == '+') ||
          (prev_token_ == kMinusMinusToken && ch == '-') ||
          (prev_token_ == '<' && ch == '!') ||
          (prev_token_ == '!' && ch == '-')) {
        InsertSpaceIfNeeded();
      }
      ChangeToken(ch);
      output_.push_back(ch);
      ++index_;
    }
  }
}

template<typename OutputConsumer>
OutputConsumer* Minifier<OutputConsumer>::GetOutput() {
  Minify();
  if (!error_) {
    return &output_;
  }
  return NULL;
}

}  // namespace

namespace pagespeed {

namespace js {

bool MinifyJs(const base::StringPiece& input, std::string* out) {
  Minifier<StringConsumer> minifier(input, out);
  return (minifier.GetOutput() != NULL);
}

bool GetMinifiedJsSize(const base::StringPiece& input, int* minimized_size) {
  Minifier<SizeConsumer> minifier(input, NULL);
  SizeConsumer* output = minifier.GetOutput();
  if (output) {
    *minimized_size = output->size_;
    return true;
  } else {
    return false;
  }
}

bool MinifyJsAndCollapseStrings(const base::StringPiece& input,
                               std::string* out) {
  Minifier<StringConsumer> minifier(input, out);
  minifier.EnableStringCollapse();
  return (minifier.GetOutput() != NULL);
}

bool GetMinifiedStringCollapsedJsSize(const base::StringPiece& input,
                                      int* minimized_size) {
  Minifier<SizeConsumer> minifier(input, NULL);
  minifier.EnableStringCollapse();
  SizeConsumer* output = minifier.GetOutput();
  if (output) {
    *minimized_size = output->size_;
    return true;
  } else {
    return false;
  }
}

}  // namespace js

}  // namespace pagespeed
