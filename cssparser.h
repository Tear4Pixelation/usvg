#pragma once

// CSS parser based on https://github.com/litehtml/litehtml (BSD license)

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

enum attr_select_condition
{
  select_exists,
  select_equal,
  select_contain_str,
  select_start_str,
  select_end_str,
  select_pseudo_class,
  select_pseudo_element,
};

struct css_attribute_selector
{
  // which of the attributes element_select() special-cases, resolved from `attribute` once at parse time
  //  instead of comparing strings for every rule tested against every node
  enum Kind { OTHER, CLASS, ID };

  std::string attribute;
  std::string val;
  //std::vector<std::string>    class_val;
  attr_select_condition condition = select_exists;
  Kind kind = OTHER;
};

class css_element_selector
{
public:
  std::string m_tag;
  std::vector<css_attribute_selector> m_attrs;
  bool m_anyTag = true;  // m_tag empty or "*", set by parse()
  void parse(const std::string& txt);
};

enum css_combinator {combinator_descendant, combinator_child, combinator_adjacent_sibling, combinator_general_sibling};

class css_selector
{
public:
  css_element_selector m_right;
  std::unique_ptr<css_selector> m_left;
  css_combinator m_combinator = combinator_descendant;

  bool parse(const std::string& text);
  int calc_specificity();
};

// subclass this to store declarations (passed to parseDecl) and provide fns needed for selector matching
class css_declarations
{
public:
  virtual ~css_declarations() {}
  virtual bool isTag(void* el, const char* tag) const = 0;
  virtual bool hasClass(void* el, const char* cls) const = 0;
  virtual bool hasId(void* el, const char* id) const = 0;
  virtual const char* attribute(void* el, const char* name) const = 0;
  virtual void* parent(void* el) const = 0;
  virtual void parseDecl(const char* name, const char* value) = 0;
};

// rule = selector + declaration block
class css_rule
{
public:
  std::unique_ptr<css_selector> m_selector;
  std::unique_ptr<css_declarations> m_decls;
  int m_specificity = 0;
  int m_order = 0;

  css_rule(css_selector* sel, css_declarations* decls, int order)
    : m_selector(sel), m_decls(decls), m_specificity(sel->calc_specificity()), m_order(order) {}
  const css_declarations* decls() const { return m_decls.get(); }
  void parse_declarations(const std::string& stylestr);
  bool select(void* el) const { return select(el, *m_selector); }

private:
  bool select(void* el, const css_selector& selector) const;
  bool select_ancestor(void* el, const css_selector& selector) const;
  bool element_select(void* el, const css_element_selector& selector) const;
};

// pass stylesheet(s) as text to parse_stylesheet(), then call sort_rules() after last stylesheet
class css_stylesheet
{
public:
  virtual ~css_stylesheet() {}
  virtual css_declarations* createCssDecls() = 0;
  const std::vector<css_rule>& rules() const { return m_rules; }
  void parse_stylesheet(const char* str);  //, const char* baseurl);
  void sort_rules();

  // Rule index, built by sort_rules().  Every rule is filed under one thing its rightmost compound
  //  selector requires - an id, else its rarest class, else its tag - or under `always` if it requires
  //  none of those.  A rule can then only match an element if it is in the bucket of one of the
  //  element's classes, its id or its tag, or in `always`.  The index only narrows which rules are
  //  tested: callers gather the candidate indices, sort them and still run css_rule::select() on each,
  //  in rules() order, so the cascade is exactly what testing every rule gives.
  bool has_rule_index() const { return m_indexValid; }
  const std::vector<int>* rules_for_class(const std::string& cls) const { return lookup(m_byClass, cls); }
  const std::vector<int>* rules_for_id(const std::string& id) const { return lookup(m_byId, id); }
  const std::vector<int>* rules_for_tag(const std::string& tag) const { return lookup(m_byTag, tag); }
  const std::vector<int>& rules_always() const { return m_always; }

private:
  typedef std::unordered_map<std::string, std::vector<int>> RuleBuckets;

  std::vector<css_rule> m_rules;
  RuleBuckets m_byClass, m_byId, m_byTag;
  std::vector<int> m_always;
  bool m_indexValid = false;

  static const std::vector<int>* lookup(const RuleBuckets& buckets, const std::string& key)
  {
    auto it = buckets.find(key);
    return it == buckets.end() ? nullptr : &it->second;
  }
  void build_rule_index();

  void parse_selectors(const std::string& txt, const std::string& styles);
};
