#pragma once

#include <vector>
#include <memory>

#include "pugixml.hpp"
// don't want to fork pugi repo on account of two lines!
#if !defined(PUGIXML_NO_XPATH) || !defined(PUGIXML_NO_EXCEPTIONS)
#error PUGIXML_NO_XPATH and PUGIXML_NO_EXCEPTIONS must be defined - edit pugiconfig.hpp or use -D switch
#endif

#include "ulib/stringutil.h"
#include "ulib/platformutil.h"
#include "ulib/fileutil.h"

struct PugiXMLWriter : public pugi::xml_writer
{
  IOStream& strm;
  PugiXMLWriter(IOStream& _strm) : strm(_strm) {}
  void write(const void* data, size_t size) override { strm.write(data, size); }
};

class XmlFragment
{
public:
  pugi::xml_document doc;
  XmlFragment(const pugi::xml_node& node) { doc.append_copy(node); }
  XmlFragment* clone() const { return new XmlFragment(doc.first_child()); }
  const char* name() const { return doc.first_child().name(); }
};

// TODO: remove "write" prefix from each method
// XmlStreamWriter writes the XML text as the caller visits the tree - no DOM is built.  The output is
//  byte-identical to what pugixml's doc.save(..., indent, format_default | format_no_declaration) printed
//  when this class built a pugi::xml_document first (saved documents, sync messages and undo persistence
//  all depend on that), so the rules below mirror pugi's node_output() exactly:
//  - an element starts on a new line, indented by its depth, unless it follows text (pugi indent_flags);
//  - a childless element is closed with " />", one with children with "</name>" on its own line, or right
//    after its text if text was the last child;
//  - attribute values escape & < > " and control characters except \t (so \n and \r too); text escapes
//    & < > and control characters except \t \n \r; a control character becomes &#NN; with two digits;
//  - a trailing newline follows the last top-level element.
// All attributes of an element must be written before its first child or text (true of every caller -
//  pugi allowed otherwise, a stream cannot).  Constructed with an IOStream, output goes straight to it
//  (in chunks); otherwise it is buffered until save().
class XmlStreamWriter
{
  enum { INDENT_NEWLINE = 1, INDENT_INDENT = 2 };
  static constexpr size_t FLUSH_SIZE = 1 << 16;

  IOStream* out = NULL;
  std::string buf;
  std::string indent = "  ";
  std::vector<std::string> openNames;
  bool tagOpen = false;  // start tag of openNames.back() written without its '>' - no children yet
  int indentFlags = INDENT_INDENT;

  bool writeStyleAttr = false;
  std::string styleAttr;

  std::vector<char> temp;

  // pugi's chartypex_table: bit 1 = special in text (ctx_special_pcdata), bit 2 = special in attribute
  static const unsigned char* escapeTable()
  {
    static unsigned char table[256] = {0};
    static bool init = [](){
      for(int ch = 0; ch < 32; ++ch)
        table[ch] = (ch == 9 ? 0 : 2) | (ch == 9 || ch == 10 || ch == 13 ? 0 : 1);
      table[(unsigned char)'&'] = table[(unsigned char)'<'] = table[(unsigned char)'>'] = 3;
      table[(unsigned char)'"'] = 2;
      return true;
    }();
    (void)init;
    return table;
  }

  void writeEscaped(const char* s, unsigned char mask)
  {
    const unsigned char* table = escapeTable();
    for(;;) {
      const char* start = s;
      while(*s && !(table[(unsigned char)*s] & mask)) ++s;
      buf.append(start, s - start);
      if(!*s) break;
      switch(*s) {
      case '&': buf.append("&amp;"); break;
      case '<': buf.append("&lt;"); break;
      case '>': buf.append("&gt;"); break;
      case '"': buf.append("&quot;"); break;
      default: {
        unsigned int ch = (unsigned char)*s;
        char esc[5] = {'&', '#', char('0' + ch/10), char('0' + ch%10), ';'};
        buf.append(esc, 5);
      }
      }
      ++s;
    }
  }

  void writeIndent(size_t depth) { for(size_t ii = 0; ii < depth; ++ii) buf.append(indent); }

  // a child (element, text or fragment) is about to be written to the current element
  void openChild() { if(tagOpen) { buf.push_back('>'); tagOpen = false; } }

  void maybeFlush() { if(out && buf.size() >= FLUSH_SIZE) { out->write(buf.data(), buf.size()); buf.clear(); } }

  struct StringXMLWriter : public pugi::xml_writer
  {
    std::string& str;
    StringXMLWriter(std::string& _str) : str(_str) {}
    void write(const void* data, size_t size) override { str.append((const char*)data, size); }
  };

  void finish()
  {
    while(!openNames.empty())
      writeEndElement();
    if(indentFlags & INDENT_NEWLINE)
      buf.push_back('\n');
    indentFlags = 0;  // so a second save() adds nothing more
  }

public:
  int defaultFloatPrecision = 3;

  XmlStreamWriter() : temp(1024) {}
  // stream output straight to strm; indent must be the one save() would have been given
  explicit XmlStreamWriter(IOStream& strm, const char* _indent = "  ") : out(&strm), indent(_indent), temp(1024) {}

  XmlStreamWriter& writeStartElement(const char* name)
  {
    openChild();
    if(indentFlags & INDENT_NEWLINE) buf.push_back('\n');
    if(indentFlags & INDENT_INDENT) writeIndent(openNames.size());
    buf.push_back('<');
    buf.append(name);
    openNames.emplace_back(name);
    tagOpen = true;
    indentFlags = INDENT_NEWLINE | INDENT_INDENT;
    return *this;
  }

  XmlStreamWriter& writeEndElement()
  {
    if(openNames.empty()) return *this;
    if(tagOpen) {
      buf.append(" />");
      tagOpen = false;
    }
    else {
      if(indentFlags & INDENT_NEWLINE) buf.push_back('\n');
      if(indentFlags & INDENT_INDENT) writeIndent(openNames.size() - 1);
      buf.append("</");
      buf.append(openNames.back());
      buf.push_back('>');
    }
    openNames.pop_back();
    indentFlags = INDENT_NEWLINE | INDENT_INDENT;
    maybeFlush();
    return *this;
  }

  XmlStreamWriter& writeAttribute(const char* name, const char* value)
  {
    if(writeStyleAttr) {
      styleAttr.append(name); styleAttr.append(":"); styleAttr.append(value); styleAttr.append(";");
    }
    else {
      ASSERT(tagOpen && "XmlStreamWriter: attribute written after the element's first child");
      if(!tagOpen) return *this;
      buf.push_back(' ');
      buf.append(name);
      buf.append("=\"");
      writeEscaped(value, 2);
      buf.push_back('"');
    }
    return *this;
  }

  XmlStreamWriter& writeAttribute(const char* name, const std::string& value)
  {
    return writeAttribute(name, value.c_str());
  }

  XmlStreamWriter& writeAttribute(const char* name, double value, int precision = -1)
  {
    // if value is an int, print as an int
    if(int(value) == value)
      return writeAttribute(name, int(value));

    char* buff = temp.data();
    int ii = realToStr(buff, value, precision < 0 ? defaultFloatPrecision : precision);
    buff[ii] = '\0';
    return writeAttribute(name, buff);
  }

  XmlStreamWriter& writeAttribute(const char* name, int value)
  {
    char* buff = temp.data();
    int ii = intToStr(buff, value);
    buff[ii] = '\0';
    return writeAttribute(name, buff);
  }

  XmlStreamWriter& writeCharacters(const char* text)
  {
    openChild();
    writeEscaped(text, 1);
    indentFlags = 0;
    maybeFlush();
    return *this;
  }

  XmlStreamWriter& writeFragment(const XmlFragment& fragment)
  {
    pugi::xml_node node = fragment.doc.first_child();
    if(!node) return *this;  // appending a null copy added nothing
    openChild();
    // pugi prints a lone node with its own indent first and a newline after; inline, the newline comes
    //  before it instead and the indent only when the preceding sibling was not text
    std::string str;
    StringXMLWriter writer(str);
    size_t depth = openNames.size();
    node.print(writer, indent.c_str(), pugi::format_default, pugi::encoding_auto, depth);
    if(node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) {
      buf.append(str);
      indentFlags = 0;
    }
    else {
      size_t skip = std::min(str.size(), indent.size()*depth);
      if(!str.empty() && str.back() == '\n') str.pop_back();
      if(indentFlags & INDENT_NEWLINE) buf.push_back('\n');
      if(indentFlags & INDENT_INDENT) buf.append(str);
      else buf.append(str, skip, std::string::npos);
      indentFlags = INDENT_NEWLINE | INDENT_INDENT;
    }
    maybeFlush();
    return *this;
  }

  XmlStreamWriter& startStyleAttribute() { writeStyleAttr = true; return *this; }
  XmlStreamWriter& endStyleAttribute(const char* attribname = "style")
  {
    writeStyleAttr = false;
    if(!styleAttr.empty())
      writeAttribute(attribname, styleAttr.c_str());
    styleAttr.clear();
    return *this;
  }

  char* getTemp(size_t reserve = 0) { if(reserve > temp.size()) { temp.resize(reserve); } return temp.data(); }
  void saveFile(const char* filename, const char* _indent = "  ")
  {
    FileStream strm(filename, "wb");
    save(strm, _indent);  // let FileStream handle unicode on Windows instead of using pugi save_file wstring
  }

  void save(std::ostream& strm, const char* _indent = "  ")
  {
    ASSERT(!out && indent == _indent && "XmlStreamWriter: indent is fixed when the writer is constructed");
    finish();
    strm.write(buf.data(), buf.size());
  }

  void save(IOStream& strm, const char* _indent = "  ")
  {
    ASSERT((!out || out == &strm) && indent == _indent && "XmlStreamWriter: indent is fixed when the writer is constructed");
    finish();
    strm.write(buf.data(), buf.size());
    if(out)
      buf.clear();
  }
};

class XmlStreamAttribute
{
  pugi::xml_attribute attr;
public:
  XmlStreamAttribute(pugi::xml_attribute _attr) : attr(_attr) {}
  const char* name() const { return attr.name(); }
  const char* value() const { return attr.as_string(); }
  XmlStreamAttribute next() const { return XmlStreamAttribute(attr.next_attribute()); }
  operator bool() { return attr; }
};

// TODO: consider requiring that caller manage 'doc' instead of us!
class XmlStreamAttributes
{
protected:
  pugi::xml_node node;
  std::unique_ptr<pugi::xml_document> doc;
public:
  XmlStreamAttributes(pugi::xml_node _node) : node(_node), doc(nullptr) {}
  XmlStreamAttributes() : doc(new pugi::xml_document()) { node = doc->append_child("dummy"); }
  XmlStreamAttributes(XmlStreamAttributes&&) = default;

  const char* value(const char* name) const { return node.attribute(name).as_string(); }
  void append(const char* name, const char* value) { node.append_attribute(name).set_value(value); }
  XmlStreamAttribute firstAttribute() const { return XmlStreamAttribute(node.first_attribute()); }
};

class XmlStreamReader
{
  pugi::xml_document doc;
  pugi::xml_node topNode;
  pugi::xml_parse_result parseResult;
  std::vector<pugi::xml_node> nodes;
  bool starting;

public:
  enum TokenType {NoToken=0, StartDocument, EndDocument,
      StartElement, EndElement, CData, ProcessingInstruction, Comment, Other};
  enum { ParseDefault = pugi::parse_default, BufferInPlace = 0x10000000 };

  XmlStreamReader(const char* data, int len, unsigned int opts = ParseDefault) : starting(true)
  {
    parseResult = opts & BufferInPlace ?
        doc.load_buffer_inplace((void*)data, len, (opts & ~BufferInPlace)) : doc.load_buffer(data, len, opts);
    topNode = doc;
  }

  XmlStreamReader(std::istream& strm, unsigned int opts = ParseDefault) : starting(true)
  {
    doc.load(strm, opts);
    topNode = doc;
  }

  // for processing only a subtree of a document
  XmlStreamReader(const pugi::xml_node& node) : topNode(node), starting(true) {}

  int parseStatus() { return parseResult.status; }
  bool atEnd() { return tokenType() == EndDocument; }
  const char* name() { return nodes.back().name(); }
  const char* text() { return nodes.back().value(); }
  XmlStreamAttributes attributes() { return XmlStreamAttributes(nodes.back()); }

  // this can be used to store unrecognized nodes
  XmlFragment* readNodeAsFragment()
  {
    // this will cause next call to readNext() to advance to next sibling of current node
    starting = false;
    return nodes.empty() ? new XmlFragment(pugi::xml_node()) : new XmlFragment(nodes.back());
  }

  // depth-first traversal of doc; rules: elements on stack are always valid (i.e. never null)
  TokenType readNext()
  {
    if(nodes.empty()) {
      nodes.push_back(topNode);
    }
    else if(starting && nodes.back().first_child()) {
      nodes.push_back(nodes.back().first_child());
      starting = true;
    }
    else if(starting && nodes.back().type() == pugi::node_element)
      starting = false;
    else {
      // this should handle both node_element types and other node types with never have sibilings
      pugi::xml_node next = nodes.back().next_sibling();
      if(next) {
        nodes.back() = next;
        starting = true;
      }
      else {
        if(nodes.size() > 1)
          nodes.pop_back();
        starting = false;
      }
    }
    //PLATFORM_LOG("%s for %s", tokenNames[int(tokenType())], nodes.back().name());
    return tokenType();
  }

  TokenType tokenType()
  {
    if(nodes.empty())
      return NoToken;
    if(nodes.size() == 1)
      return starting ? StartDocument : EndDocument;
    pugi::xml_node_type type = nodes.back().type();
    if(type == pugi::node_element)
      return starting ? StartElement : EndElement;
    else if(type == pugi::node_pcdata || type == pugi::node_cdata)
      return CData;
    else if(type == pugi::node_pi)
      return ProcessingInstruction;
    else if(type == pugi::node_comment)
      return Comment;
    else
      return Other;
  }
};
