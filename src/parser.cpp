// sass.hpp must go before all system headers to get the
// __EXTENSIONS__ fix on Solaris.
#include "sass.hpp"

#include "parser.hpp"
#include "color_maps.hpp"
#include "util_string.hpp"

// Notes about delayed: some ast nodes can have delayed evaluation so
// they can preserve their original semantics if needed. This is most
// prominently exhibited by the division operation, since it is not
// only a valid operation, but also a valid css statement (i.e. for
// fonts, as in `16px/24px`). When parsing lists and expression we
// unwrap single items from lists and other operations. A nested list
// must not be delayed, only the items of the first level sometimes
// are delayed (as with argument lists). To achieve this we need to
// pass status to the list parser, so this can be set correctly.
// Another case with delayed values are colors. In compressed mode
// only processed values get compressed (other are left as written).


namespace Sass {
  using namespace Constants;
  using namespace Prelexer;

  Parser Parser::from_c_str(const char* beg, Context& ctx, Backtraces traces, ParserState pstate, const char* source, bool allow_parent)
  {
    pstate.offset.column = 0;
    pstate.offset.line = 0;
    Parser p(ctx, pstate, traces, allow_parent);
    p.source   = source ? source : beg;
    p.position = beg ? beg : p.source;
    p.end      = p.position + strlen(p.position);
    Block_Obj root = SASS_MEMORY_NEW(Block, pstate);
    p.block_stack.push_back(root);
    root->is_root(true);
    return p;
  }

  Parser Parser::from_c_str(const char* beg, const char* end, Context& ctx, Backtraces traces, ParserState pstate, const char* source, bool allow_parent)
  {
    pstate.offset.column = 0;
    pstate.offset.line = 0;
    Parser p(ctx, pstate, traces, allow_parent);
    p.source   = source ? source : beg;
    p.position = beg ? beg : p.source;
    p.end      = end ? end : p.position + strlen(p.position);
    Block_Obj root = SASS_MEMORY_NEW(Block, pstate);
    p.block_stack.push_back(root);
    root->is_root(true);
    return p;
  }

   void Parser::advanceToNextToken() {
      lex < css_comments >(false);
      // advance to position
      pstate += pstate.offset;
      pstate.offset.column = 0;
      pstate.offset.line = 0;
    }

  SelectorListObj Parser::parse_selector(const char* beg, Context& ctx, Backtraces traces, ParserState pstate, const char* source, bool allow_parent)
  {
    Parser p = Parser::from_c_str(beg, ctx, traces, pstate, source, allow_parent);
    // ToDo: remap the source-map entries somehow
    return p.parseSelectorList(false);
  }

  bool Parser::peek_newline(const char* start)
  {
    return peek_linefeed(start ? start : position)
           && ! peek_css<exactly<'{'>>(start);
  }

  Parser Parser::from_token(Token t, Context& ctx, Backtraces traces, ParserState pstate, const char* source)
  {
    Parser p(ctx, pstate, traces);
    p.source   = source ? source : t.begin;
    p.position = t.begin ? t.begin : p.source;
    p.end      = t.end ? t.end : p.position + strlen(p.position);
    Block_Obj root = SASS_MEMORY_NEW(Block, pstate);
    p.block_stack.push_back(root);
    root->is_root(true);
    return p;
  }

  /* main entry point to parse root block */
  Block_Obj Parser::parse()
  {

    // consume unicode BOM
    read_bom();

    // scan the input to find invalid utf8 sequences
    const char* it = utf8::find_invalid(position, end);

    // report invalid utf8
    if (it != end) {
      pstate += Offset::init(position, it);
      traces.push_back(Backtrace(pstate));
      throw Exception::InvalidSass(pstate, traces, "Invalid UTF-8 sequence");
    }

    // create a block AST node to hold children
    Block_Obj root = SASS_MEMORY_NEW(Block, pstate, 0, true);

    // check seems a bit esoteric but works
    if (ctx.resources.size() == 1) {
      // apply headers only on very first include
      ctx.apply_custom_headers(root, path, pstate);
    }

    // parse children nodes
    block_stack.push_back(root);
    parse_block_nodes(true);
    block_stack.pop_back();

    // update final position
    root->update_pstate(pstate);

    if (position != end) {
      css_error("Invalid CSS", " after ", ": expected selector or at-rule, was ");
    }

    return root;
  }


  // convenience function for block parsing
  // will create a new block ad-hoc for you
  // this is the base block parsing function
  Block_Obj Parser::parse_css_block(bool is_root)
  {

    // parse comments before block
    // lex < optional_css_comments >();

    // lex mandatory opener or error out
    if (!lex_css < exactly<'{'> >()) {
      css_error("Invalid CSS", " after ", ": expected \"{\", was ");
    }
    // create new block and push to the selector stack
    Block_Obj block = SASS_MEMORY_NEW(Block, pstate, 0, is_root);
    block_stack.push_back(block);

    if (!parse_block_nodes(is_root)) css_error("Invalid CSS", " after ", ": expected \"}\", was ");

    if (!lex_css < exactly<'}'> >()) {
      css_error("Invalid CSS", " after ", ": expected \"}\", was ");
    }

    // update for end position
    // this seems to be done somewhere else
    // but that fixed selector schema issue
    // block->update_pstate(pstate);

    // parse comments after block
    // lex < optional_css_comments >();

    block_stack.pop_back();

    return block;
  }

  // convenience function for block parsing
  // will create a new block ad-hoc for you
  // also updates the `in_at_root` flag
  Block_Obj Parser::parse_block(bool is_root)
  {
    return parse_css_block(is_root);
  }

  // the main block parsing function
  // parses stuff between `{` and `}`
  bool Parser::parse_block_nodes(bool is_root)
  {

    // loop until end of string
    while (position < end) {

      // we should be able to refactor this
      parse_block_comments();
      lex < css_whitespace >();

      if (lex < exactly<';'> >()) continue;
      if (peek < end_of_file >()) return true;
      if (peek < exactly<'}'> >()) return true;

      if (parse_block_node(is_root)) continue;

      parse_block_comments();

      if (lex_css < exactly<';'> >()) continue;
      if (peek_css < end_of_file >()) return true;
      if (peek_css < exactly<'}'> >()) return true;

      // illegal sass
      return false;
    }
    // return success
    return true;
  }

  // parser for a single node in a block
  // semicolons must be lexed beforehand
  bool Parser::parse_block_node(bool is_root) {

    Block_Obj block = block_stack.back();

    parse_block_comments();

    // throw away white-space
    // includes line comments
    lex < css_whitespace >();

    Lookahead lookahead_result;

    // also parse block comments

    // first parse everything that is allowed in functions
    if (lex < variable >(true)) { block->append(parse_assignment()); }
    else if (lex < kwd_err >(true)) { block->append(parse_error()); }
    else if (lex < kwd_dbg >(true)) { block->append(parse_debug()); }
    else if (lex < kwd_warn >(true)) { block->append(parse_warning()); }
    else if (lex < kwd_if_directive >(true)) { block->append(parse_if_directive()); }
    else if (lex < kwd_for_directive >(true)) { block->append(parse_for_directive()); }
    else if (lex < kwd_each_directive >(true)) { block->append(parse_each_directive()); }
    else if (lex < kwd_while_directive >(true)) { block->append(parse_while_directive()); }
    else if (lex < kwd_return_directive >(true)) { block->append(parse_return_directive()); }

    // parse imports to process later
    else if (lex < kwd_import >(true)) {
      Scope parent = stack.empty() ? Scope::Rules : stack.back();
      if (parent != Scope::Function && parent != Scope::Root && parent != Scope::Rules && parent != Scope::Media) {
        if (! peek_css< uri_prefix >(position)) { // this seems to go in ruby sass 3.4.20
          error("Import directives may not be used within control directives or mixins.");
        }
      }
      // this puts the parsed doc into sheets
      // import stub will fetch this in expand
      Import_Obj imp = parse_import();
      // if it is a url, we only add the statement
      if (!imp->urls().empty()) block->append(imp);
      // process all resources now (add Import_Stub nodes)
      for (size_t i = 0, S = imp->incs().size(); i < S; ++i) {
        block->append(SASS_MEMORY_NEW(Import_Stub, pstate, imp->incs()[i]));
      }
    }

    else if (lex < kwd_extend >(true)) {
      Lookahead lookahead = lookahead_for_include(position);
      if (!lookahead.found) css_error("Invalid CSS", " after ", ": expected selector, was ");
      SelectorListObj target;
      if (!lookahead.has_interpolants) {
        LOCAL_FLAG(allow_parent, false);
        auto selector = parseSelectorList(true);
        auto extender = SASS_MEMORY_NEW(ExtendRule, pstate, selector);
        extender->isOptional(selector && selector->is_optional());
        block->append(extender);
      }
      else {
        LOCAL_FLAG(allow_parent, false);
        auto selector = parse_selector_schema(lookahead.found, true);
        auto extender = SASS_MEMORY_NEW(ExtendRule, pstate, selector);
        // A schema is not optional yet, check once it is evaluated
        // extender->isOptional(selector && selector->is_optional());
        block->append(extender);
      }

    }

    // selector may contain interpolations which need delayed evaluation
    else if (
      !(lookahead_result = lookahead_for_selector(position)).error &&
      !lookahead_result.is_custom_property
    )
    {
      block->append(parse_ruleset(lookahead_result));
    }

    // parse multiple specific keyword directives
    else if (lex < kwd_media >(true)) { block->append(parseMediaRule()); }
    else if (lex < kwd_at_root >(true)) { block->append(parse_at_root_block()); }
    else if (lex < kwd_include_directive >(true)) { block->append(parse_include_directive()); }
    else if (lex < kwd_content_directive >(true)) { block->append(parse_content_directive()); }
    else if (lex < kwd_supports_directive >(true)) { block->append(parse_supports_directive()); }
    else if (lex < kwd_mixin >(true)) { block->append(parse_definition(Definition::MIXIN)); }
    else if (lex < kwd_function >(true)) { block->append(parse_definition(Definition::FUNCTION)); }

    // ignore the @charset directive for now
    else if (lex< kwd_charset_directive >(true)) { parse_charset_directive(); }

    else if (lex < exactly < else_kwd >>(true)) { error("Invalid CSS: @else must come after @if"); }

    // generic at keyword (keep last)
    else if (lex< at_keyword >(true)) { block->append(parse_directive()); }

    else if (is_root && stack.back() != Scope::AtRoot /* && block->is_root() */) {
      lex< css_whitespace >();
      if (position >= end) return true;
      css_error("Invalid CSS", " after ", ": expected 1 selector or at-rule, was ");
    }
    // parse a declaration
    else
    {
      // ToDo: how does it handle parse errors?
      // maybe we are expected to parse something?
      Declaration_Obj decl = parse_declaration();
      decl->tabs(indentation);
      block->append(decl);
      // maybe we have a "sub-block"
      if (peek< exactly<'{'> >()) {
        if (decl->is_indented()) ++ indentation;
        // parse a propset that rides on the declaration's property
        stack.push_back(Scope::Properties);
        decl->block(parse_block());
        stack.pop_back();
        if (decl->is_indented()) -- indentation;
      }
    }
    // something matched
    return true;
  }
  // EO parse_block_nodes

  // parse imports inside the
  Import_Obj Parser::parse_import()
  {
    Import_Obj imp = SASS_MEMORY_NEW(Import, pstate);
    std::vector<std::pair<std::string,Function_Call_Obj>> to_import;
    bool first = true;
    do {
      while (lex< block_comment >());
      if (lex< quoted_string >()) {
        to_import.push_back(std::pair<std::string,Function_Call_Obj>(std::string(lexed), {}));
      }
      else if (lex< uri_prefix >()) {
        Arguments_Obj args = SASS_MEMORY_NEW(Arguments, pstate);
        Function_Call_Obj result = SASS_MEMORY_NEW(Function_Call, pstate, std::string("url"), args);

        if (lex< quoted_string >()) {
          Expression_Obj quoted_url = parse_string();
          args->append(SASS_MEMORY_NEW(Argument, quoted_url->pstate(), quoted_url));
        }
        else if (String_Obj string_url = parse_url_function_argument()) {
          args->append(SASS_MEMORY_NEW(Argument, string_url->pstate(), string_url));
        }
        else if (peek < skip_over_scopes < exactly < '(' >, exactly < ')' > > >(position)) {
          Expression_Obj braced_url = parse_list(); // parse_interpolated_chunk(lexed);
          args->append(SASS_MEMORY_NEW(Argument, braced_url->pstate(), braced_url));
        }
        else {
          error("malformed URL");
        }
        if (!lex< exactly<')'> >()) error("URI is missing ')'");
        to_import.push_back(std::pair<std::string, Function_Call_Obj>("", result));
      }
      else {
        if (first) error("@import directive requires a url or quoted path");
        else error("expecting another url or quoted path in @import list");
      }
      first = false;
    } while (lex_css< exactly<','> >());

    if (!peek_css< alternatives< exactly<';'>, exactly<'}'>, end_of_file > >()) {
      List_Obj import_queries = parse_media_queries();
      imp->import_queries(import_queries);
    }

    for(auto location : to_import) {
      if (location.second) {
        imp->urls().push_back(location.second);
      }
      // check if custom importers want to take over the handling
      else if (!ctx.call_importers(unquote(location.first), path, pstate, imp)) {
        // nobody wants it, so we do our import
        ctx.import_url(imp, location.first, path);
      }
    }

    return imp;
  }

  Definition_Obj Parser::parse_definition(Definition::Type which_type)
  {
    std::string which_str(lexed);
    if (!lex< identifier >()) error("invalid name in " + which_str + " definition");
    std::string name(Util::normalize_underscores(lexed));
    if (which_type == Definition::FUNCTION && (name == "and" || name == "or" || name == "not"))
    { error("Invalid function name \"" + name + "\"."); }
    ParserState source_position_of_def = pstate;
    Parameters_Obj params = parse_parameters();
    if (which_type == Definition::MIXIN) stack.push_back(Scope::Mixin);
    else stack.push_back(Scope::Function);
    Block_Obj body = parse_block();
    stack.pop_back();
    return SASS_MEMORY_NEW(Definition, source_position_of_def, name, params, body, which_type);
  }

  Parameters_Obj Parser::parse_parameters()
  {
    Parameters_Obj params = SASS_MEMORY_NEW(Parameters, pstate);
    if (lex_css< exactly<'('> >()) {
      // if there's anything there at all
      if (!peek_css< exactly<')'> >()) {
        do {
          if (peek< exactly<')'> >()) break;
          params->append(parse_parameter());
        } while (lex_css< exactly<','> >());
      }
      if (!lex_css< exactly<')'> >()) {
        css_error("Invalid CSS", " after ", ": expected \")\", was ");
      }
    }
    return params;
  }

  Parameter_Obj Parser::parse_parameter()
  {
    if (peek< alternatives< exactly<','>, exactly< '{' >, exactly<';'> > >()) {
      css_error("Invalid CSS", " after ", ": expected variable (e.g. $foo), was ");
    }
    while (lex< alternatives < spaces, block_comment > >());
    lex < variable >();
    std::string name(Util::normalize_underscores(lexed));
    ParserState pos = pstate;
    Expression_Obj val;
    bool is_rest = false;
    while (lex< alternatives < spaces, block_comment > >());
    if (lex< exactly<':'> >()) { // there's a default value
      while (lex< block_comment >());
      val = parse_space_list();
    }
    else if (lex< exactly< ellipsis > >()) {
      is_rest = true;
    }
    return SASS_MEMORY_NEW(Parameter, pos, name, val, is_rest);
  }

  Arguments_Obj Parser::parse_arguments()
  {
    Arguments_Obj args = SASS_MEMORY_NEW(Arguments, pstate);
    if (lex_css< exactly<'('> >()) {
      // if there's anything there at all
      if (!peek_css< exactly<')'> >()) {
        do {
          if (peek< exactly<')'> >()) break;
          args->append(parse_argument());
        } while (lex_css< exactly<','> >());
      }
      if (!lex_css< exactly<')'> >()) {
        css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was ");
      }
    }
    return args;
  }

  Argument_Obj Parser::parse_argument()
  {
    if (peek< alternatives< exactly<','>, exactly< '{' >, exactly<';'> > >()) {
      css_error("Invalid CSS", " after ", ": expected \")\", was ");
    }
    if (peek_css< sequence < exactly< hash_lbrace >, exactly< rbrace > > >()) {
      position += 2;
      css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was ");
    }

    Argument_Obj arg;
    if (peek_css< sequence < variable, optional_css_comments, exactly<':'> > >()) {
      lex_css< variable >();
      std::string name(Util::normalize_underscores(lexed));
      ParserState p = pstate;
      lex_css< exactly<':'> >();
      Expression_Obj val = parse_space_list();
      arg = SASS_MEMORY_NEW(Argument, p, val, name);
    }
    else {
      bool is_arglist = false;
      bool is_keyword = false;
      Expression_Obj val = parse_space_list();
      List* l = Cast<List>(val);
      if (lex_css< exactly< ellipsis > >()) {
        if (val->concrete_type() == Expression::MAP || (
           (l != NULL && l->separator() == SASS_HASH)
        )) is_keyword = true;
        else is_arglist = true;
      }
      arg = SASS_MEMORY_NEW(Argument, pstate, val, "", is_arglist, is_keyword);
    }
    return arg;
  }

  Assignment_Obj Parser::parse_assignment()
  {
    std::string name(Util::normalize_underscores(lexed));
    ParserState var_source_position = pstate;
    if (!lex< exactly<':'> >()) error("expected ':' after " + name + " in assignment statement");
    if (peek_css< alternatives < exactly<';'>, end_of_file > >()) {
      css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was ");
    }
    Expression_Obj val;
    Lookahead lookahead = lookahead_for_value(position);
    if (lookahead.has_interpolants && lookahead.found) {
      val = parse_value_schema(lookahead.found);
    } else {
      val = parse_list();
    }
    bool is_default = false;
    bool is_global = false;
    while (peek< alternatives < default_flag, global_flag > >()) {
      if (lex< default_flag >()) is_default = true;
      else if (lex< global_flag >()) is_global = true;
    }
    return SASS_MEMORY_NEW(Assignment, var_source_position, name, val, is_default, is_global);
  }

  // a ruleset connects a selector and a block
  Ruleset_Obj Parser::parse_ruleset(Lookahead lookahead)
  {
    NESTING_GUARD(nestings);
    // inherit is_root from parent block
    Block_Obj parent = block_stack.back();
    bool is_root = parent && parent->is_root();
    // make sure to move up the the last position
    lex < optional_css_whitespace >(false, true);
    // create the connector object (add parts later)
    Ruleset_Obj ruleset = SASS_MEMORY_NEW(Ruleset, pstate);
    // parse selector static or as schema to be evaluated later
    if (lookahead.parsable) {
      ruleset->selector(parseSelectorList(false));
    }
    else {
      SelectorListObj list = SASS_MEMORY_NEW(SelectorList, pstate);
      auto sc = parse_selector_schema(lookahead.position, false);
      ruleset->schema(sc);
      ruleset->selector(list);
    }
    // then parse the inner block
    stack.push_back(Scope::Rules);
    ruleset->block(parse_block());
    stack.pop_back();
    // update for end position
    ruleset->update_pstate(pstate);
    ruleset->block()->update_pstate(pstate);
    // need this info for sanity checks
    ruleset->is_root(is_root);
    // return AST Node
    return ruleset;
  }

  // parse a selector schema that will be evaluated in the eval stage
  // uses a string schema internally to do the actual schema handling
  // in the eval stage we will be re-parse it into an actual selector
  Selector_Schema_Obj Parser::parse_selector_schema(const char* end_of_selector, bool chroot)
  {
    NESTING_GUARD(nestings);
    // move up to the start
    lex< optional_spaces >();
    const char* i = position;
    // selector schema re-uses string schema implementation
    String_Schema* schema = SASS_MEMORY_NEW(String_Schema, pstate);
    // the selector schema is pretty much just a wrapper for the string schema
    Selector_Schema_Obj selector_schema = SASS_MEMORY_NEW(Selector_Schema, pstate, schema);
    selector_schema->connect_parent(chroot == false);

    // process until end
    while (i < end_of_selector) {
      // try to parse multiple interpolants
      if (const char* p = find_first_in_interval< exactly<hash_lbrace>, block_comment >(i, end_of_selector)) {
        // accumulate the preceding segment if the position has advanced
        if (i < p) {
          std::string parsed(i, p);
          String_Constant_Obj str = SASS_MEMORY_NEW(String_Constant, pstate, parsed);
          pstate += Offset(parsed);
          str->update_pstate(pstate);
          schema->append(str);
        }

        // skip over all nested inner interpolations up to our own delimiter
        const char* j = skip_over_scopes< exactly<hash_lbrace>, exactly<rbrace> >(p + 2, end_of_selector);
        // check if the interpolation never ends of only contains white-space (error out)
        if (!j || peek < sequence < optional_spaces, exactly<rbrace> > >(p+2)) {
          position = p+2;
          css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was ");
        }
        // pass inner expression to the parser to resolve nested interpolations
        pstate.add(p, p+2);
        Expression_Obj interpolant = Parser::from_c_str(p+2, j, ctx, traces, pstate).parse_list();
        // set status on the list expression
        interpolant->is_interpolant(true);
        // schema->has_interpolants(true);
        // add to the string schema
        schema->append(interpolant);
        // advance parser state
        pstate.add(p+2, j);
        // advance position
        i = j;
      }
      // no more interpolants have been found
      // add the last segment if there is one
      else {
        // make sure to add the last bits of the string up to the end (if any)
        if (i < end_of_selector) {
          std::string parsed(i, end_of_selector);
          String_Constant_Obj str = SASS_MEMORY_NEW(String_Constant, pstate, parsed);
          pstate += Offset(parsed);
          str->update_pstate(pstate);
          i = end_of_selector;
          schema->append(str);
        }
        // exit loop
      }
    }
    // EO until eos

    // update position
    position = i;

    // update for end position
    selector_schema->update_pstate(pstate);
    schema->update_pstate(pstate);

    after_token = before_token = pstate;

    // return parsed result
    return selector_schema.detach();
  }
  // EO parse_selector_schema

  void Parser::parse_charset_directive()
  {
    lex <
      sequence <
        quoted_string,
        optional_spaces,
        exactly <';'>
      >
    >();
  }

  // called after parsing `kwd_include_directive`
  Mixin_Call_Obj Parser::parse_include_directive()
  {
    // lex identifier into `lexed` var
    lex_identifier(); // may error out
    // normalize underscores to hyphens
    std::string name(Util::normalize_underscores(lexed));
    // create the initial mixin call object
    Mixin_Call_Obj call = SASS_MEMORY_NEW(Mixin_Call, pstate, name, {}, {}, {});
    // parse mandatory arguments
    call->arguments(parse_arguments());
    // parse using and optional block parameters
    bool has_parameters = lex< kwd_using >() != nullptr;

    if (has_parameters) {
      if (!peek< exactly<'('> >()) css_error("Invalid CSS", " after ", ": expected \"(\", was ");
    } else {
      if (peek< exactly<'('> >()) css_error("Invalid CSS", " after ", ": expected \";\", was ");
    }

    if (has_parameters) call->block_parameters(parse_parameters());

    // parse optional block
    if (peek < exactly <'{'> >()) {
      call->block(parse_block());
    }
    else if (has_parameters)  {
      css_error("Invalid CSS", " after ", ": expected \"{\", was ");
    }
    // return ast node
    return call.detach();
  }
  // EO parse_include_directive


  SimpleSelectorObj Parser::parse_simple_selector()
  {
    lex < css_comments >(false);
    if (lex< class_name >()) {
      return SASS_MEMORY_NEW(Class_Selector, pstate, lexed);
    }
    else if (lex< id_name >()) {
      return SASS_MEMORY_NEW(Id_Selector, pstate, lexed);
    }
    else if (lex< alternatives < variable, number, static_reference_combinator > >()) {
      return SASS_MEMORY_NEW(Type_Selector, pstate, lexed);
    }
    else if (peek< pseudo_not >()) {
      return parse_negated_selector2();
    }
    else if (peek< re_pseudo_selector >()) {
      return parse_pseudo_selector();
    }
    else if (peek< exactly<':'> >()) {
      return parse_pseudo_selector();
    }
    else if (lex < exactly<'['> >()) {
      return parse_attribute_selector();
    }
    else if (lex< placeholder >()) {
      return SASS_MEMORY_NEW(Placeholder_Selector, pstate, lexed);
    }
    else {
      css_error("Invalid CSS", " after ", ": expected selector, was ");
    }
    // failed
    return {};
  }

  Pseudo_Selector_Obj Parser::parse_negated_selector2()
  {
    lex< pseudo_not >();
    std::string name(lexed);
    ParserState nsource_position = pstate;
    SelectorListObj negated = parseSelectorList(true);
    if (!lex< exactly<')'> >()) {
      error("negated selector is missing ')'");
    }
    name.erase(name.size() - 1);

    Pseudo_Selector* sel = SASS_MEMORY_NEW(Pseudo_Selector, nsource_position, name.substr(1));
    sel->selector(negated);
    return sel;
  }

  // Helper to clean binominal string
  bool BothAreSpaces(char lhs, char rhs) { return isspace(lhs) && isspace(rhs); }

  // a pseudo selector often starts with one or two colons
  // it can contain more selectors inside parentheses
  SimpleSelectorObj Parser::parse_pseudo_selector() {

    // Lex one or two colon characters
    if (lex<pseudo_prefix>()) {
      std::string colons(lexed);
      // Check if it is a pseudo element
      bool element = colons.size() == 2;

      if (lex< sequence<
        // we keep the space within the name, strange enough
        // ToDo: refactor output to schedule the space for it
        // or do we really want to keep the real white-space?
        sequence< identifier, optional < block_comment >, exactly<'('> >
      > >())
      {

        std::string name(lexed);
        name.erase(name.size() - 1);
        ParserState p = pstate;

        // specially parse nth-child pseudo selectors
        if (lex_css < sequence < binomial, word_boundary >>()) {
          std::string parsed(lexed); // always compacting binominals (as dart-sass)
          parsed.erase(std::unique(parsed.begin(), parsed.end(), BothAreSpaces), parsed.end());
          String_Constant_Obj arg = SASS_MEMORY_NEW(String_Constant, pstate, parsed);
          Pseudo_Selector* pseudo = SASS_MEMORY_NEW(Pseudo_Selector, p, name, element);
          if (lex < sequence < css_whitespace, insensitive < of_kwd >>>(false)) {
            pseudo->selector(parseSelectorList(true));
          }
          pseudo->argument(arg);
          if (lex_css< exactly<')'> >()) {
            return pseudo;
          }
        }
        else {
          if (peek_css< exactly<')'>>()  && Util::equalsLiteral("nth-", name.substr(0, 4))) {
            css_error("Invalid CSS", " after ", ": expected An+B expression, was ");
          }

          std::string unvendored = Util::unvendor(name);

          if (unvendored == "not" || unvendored == "matches" || unvendored == "current"  || unvendored == "any" || unvendored == "has" || unvendored == "host" || unvendored == "host-context" || unvendored == "slotted") {
             if (SelectorListObj wrapped = parseSelectorList(true)) {
                if (wrapped && lex_css< exactly<')'> >()) {
                  Pseudo_Selector* pseudo = SASS_MEMORY_NEW(Pseudo_Selector, p, name, element);
                  pseudo->selector(wrapped);
                  return pseudo;
                }
              }
          } else {
            String_Schema_Obj arg = parse_css_variable_value();
            Pseudo_Selector* pseudo = SASS_MEMORY_NEW(Pseudo_Selector, p, name, element);
            pseudo->argument(arg);

            if (lex_css< exactly<')'> >()) {
              return pseudo;
            }
          }
        }

      }
      // EO if pseudo selector

      else if (lex < sequence< optional < pseudo_prefix >, identifier > >()) {
        return SASS_MEMORY_NEW(Pseudo_Selector, pstate, lexed, element);
      }
      else if (lex < pseudo_prefix >()) {
        css_error("Invalid CSS", " after ", ": expected pseudoclass or pseudoelement, was ");
      }

    }
    else {
      lex < identifier >(); // needed for error message?
      css_error("Invalid CSS", " after ", ": expected selector, was ");
    }


    css_error("Invalid CSS", " after ", ": expected \")\", was ");

    // unreachable statement
    return {};
  }

  const char* Parser::re_attr_sensitive_close(const char* src)
  {
    return alternatives < exactly<']'>, exactly<'/'> >(src);
  }

  const char* Parser::re_attr_insensitive_close(const char* src)
  {
    return sequence < insensitive<'i'>, re_attr_sensitive_close >(src);
  }

  Attribute_Selector_Obj Parser::parse_attribute_selector()
  {
    ParserState p = pstate;
    if (!lex_css< attribute_name >()) error("invalid attribute name in attribute selector");
    std::string name(lexed);
    if (lex_css< re_attr_sensitive_close >()) {
      return SASS_MEMORY_NEW(Attribute_Selector, p, name, "", {}, {});
    }
    else if (lex_css< re_attr_insensitive_close >()) {
      char modifier = lexed.begin[0];
      return SASS_MEMORY_NEW(Attribute_Selector, p, name, "", {}, modifier);
    }
    if (!lex_css< alternatives< exact_match, class_match, dash_match,
                                prefix_match, suffix_match, substring_match > >()) {
      error("invalid operator in attribute selector for " + name);
    }
    std::string matcher(lexed);

    String_Obj value;
    if (lex_css< identifier >()) {
      value = SASS_MEMORY_NEW(String_Constant, p, lexed);
    }
    else if (lex_css< quoted_string >()) {
      value = parse_interpolated_chunk(lexed, true); // needed!
    }
    else {
      error("expected a string constant or identifier in attribute selector for " + name);
    }

    if (lex_css< re_attr_sensitive_close >()) {
      return SASS_MEMORY_NEW(Attribute_Selector, p, name, matcher, value, 0);
    }
    else if (lex_css< re_attr_insensitive_close >()) {
      char modifier = lexed.begin[0];
      return SASS_MEMORY_NEW(Attribute_Selector, p, name, matcher, value, modifier);
    }
    error("unterminated attribute selector for " + name);
    return {}; // to satisfy compilers (error must not return)
  }

  /* parse block comment and add to block */
  void Parser::parse_block_comments(bool store)
  {
    Block_Obj block = block_stack.back();

    while (lex< block_comment >()) {
      bool is_important = lexed.begin[2] == '!';
      // flag on second param is to skip loosely over comments
      String_Obj contents = parse_interpolated_chunk(lexed, true, false);
      if (store) block->append(SASS_MEMORY_NEW(Comment, pstate, contents, is_important));
    }
  }

  Declaration_Obj Parser::parse_declaration() {
    String_Obj prop;
    bool is_custom_property = false;
    if (lex< sequence< optional< exactly<'*'> >, identifier_schema > >()) {
      const std::string property(lexed);
      is_custom_property = property.compare(0, 2, "--") == 0;
      prop = parse_identifier_schema();
    }
    else if (lex< sequence< optional< exactly<'*'> >, identifier, zero_plus< block_comment > > >()) {
      const std::string property(lexed);
      is_custom_property = property.compare(0, 2, "--") == 0;
      prop = SASS_MEMORY_NEW(String_Constant, pstate, lexed);
    }
    else {
      css_error("Invalid CSS", " after ", ": expected \"}\", was ");
    }
    bool is_indented = true;
    const std::string property(lexed);
    if (!lex_css< one_plus< exactly<':'> > >()) error("property \"" + escape_string(property)  + "\" must be followed by a ':'");
    if (!is_custom_property && match< sequence< optional_css_comments, exactly<';'> > >()) error("style declaration must contain a value");
    if (match< sequence< optional_css_comments, exactly<'{'> > >()) is_indented = false; // don't indent if value is empty
    if (is_custom_property) {
      return SASS_MEMORY_NEW(Declaration, prop->pstate(), prop, parse_css_variable_value(), false, true);
    }
    lex < css_comments >(false);
    if (peek_css< static_value >()) {
      return SASS_MEMORY_NEW(Declaration, prop->pstate(), prop, parse_static_value()/*, lex<kwd_important>()*/);
    }
    else {
      Expression_Obj value;
      Lookahead lookahead = lookahead_for_value(position);
      if (lookahead.found) {
        if (lookahead.has_interpolants) {
          value = parse_value_schema(lookahead.found);
        } else {
          value = parse_list(DELAYED);
        }
      }
      else {
        value = parse_list(DELAYED);
        if (List* list = Cast<List>(value)) {
          if (!list->is_bracketed() && list->length() == 0 && !peek< exactly <'{'> >()) {
            css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was ");
          }
        }
      }
      lex < css_comments >(false);
      Declaration_Obj decl = SASS_MEMORY_NEW(Declaration, prop->pstate(), prop, value/*, lex<kwd_important>()*/);
      decl->is_indented(is_indented);
      decl->update_pstate(pstate);
      return decl;
    }
  }

  Expression_Obj Parser::parse_map()
  {
    NESTING_GUARD(nestings);
    Expression_Obj key = parse_list();
    List_Obj map = SASS_MEMORY_NEW(List, pstate, 0, SASS_HASH);

    // it's not a map so return the lexed value as a list value
    if (!lex_css< exactly<':'> >())
    { return key; }

    List_Obj l = Cast<List>(key);
    if (l && l->separator() == SASS_COMMA) {
      css_error("Invalid CSS", " after ", ": expected \")\", was ");
    }

    Expression_Obj value = parse_space_list();

    map->append(key);
    map->append(value);

    while (lex_css< exactly<','> >())
    {
      // allow trailing commas - #495
      if (peek_css< exactly<')'> >(position))
      { break; }

      key = parse_space_list();

      if (!(lex< exactly<':'> >()))
      { css_error("Invalid CSS", " after ", ": expected \":\", was "); }

      value = parse_space_list();

      map->append(key);
      map->append(value);
    }

    ParserState ps = map->pstate();
    ps.offset = pstate - ps + pstate.offset;
    map->pstate(ps);

    return map;
  }

  Expression_Obj Parser::parse_bracket_list()
  {
    NESTING_GUARD(nestings);
    // check if we have an empty list
    // return the empty list as such
    if (peek_css< list_terminator >(position))
    {
      // return an empty list (nothing to delay)
      return SASS_MEMORY_NEW(List, pstate, 0, SASS_SPACE, false, true);
    }

    bool has_paren = peek_css< exactly<'('> >() != NULL;

    // now try to parse a space list
    Expression_Obj list = parse_space_list();
    // if it's a singleton, return it (don't wrap it)
    if (!peek_css< exactly<','> >(position)) {
      List_Obj l = Cast<List>(list);
      if (!l || l->is_bracketed() || has_paren) {
        List_Obj bracketed_list = SASS_MEMORY_NEW(List, pstate, 1, SASS_SPACE, false, true);
        bracketed_list->append(list);
        return bracketed_list;
      }
      l->is_bracketed(true);
      return l;
    }

    // if we got so far, we actually do have a comma list
    List_Obj bracketed_list = SASS_MEMORY_NEW(List, pstate, 2, SASS_COMMA, false, true);
    // wrap the first expression
    bracketed_list->append(list);

    while (lex_css< exactly<','> >())
    {
      // check for abort condition
      if (peek_css< list_terminator >(position)
      ) { break; }
      // otherwise add another expression
      bracketed_list->append(parse_space_list());
    }
    // return the list
    return bracketed_list;
  }

  // parse list returns either a space separated list,
  // a comma separated list or any bare expression found.
  // so to speak: we unwrap items from lists if possible here!
  Expression_Obj Parser::parse_list(bool delayed)
  {
    NESTING_GUARD(nestings);
    return parse_comma_list(delayed);
  }

  // will return singletons unwrapped
  Expression_Obj Parser::parse_comma_list(bool delayed)
  {
    NESTING_GUARD(nestings);
    // check if we have an empty list
    // return the empty list as such
    if (peek_css< list_terminator >(position))
    {
      // return an empty list (nothing to delay)
      return SASS_MEMORY_NEW(List, pstate, 0);
    }

    // now try to parse a space list
    Expression_Obj list = parse_space_list();
    // if it's a singleton, return it (don't wrap it)
    if (!peek_css< exactly<','> >(position)) {
      // set_delay doesn't apply to list children
      // so this will only undelay single values
      if (!delayed) list->set_delayed(false);
      return list;
    }

    // if we got so far, we actually do have a comma list
    List_Obj comma_list = SASS_MEMORY_NEW(List, pstate, 2, SASS_COMMA);
    // wrap the first expression
    comma_list->append(list);

    while (lex_css< exactly<','> >())
    {
      // check for abort condition
      if (peek_css< list_terminator >(position)
      ) { break; }
      // otherwise add another expression
      comma_list->append(parse_space_list());
    }
    // return the list
    return comma_list;
  }
  // EO parse_comma_list

  // will return singletons unwrapped
  Expression_Obj Parser::parse_space_list()
  {
    NESTING_GUARD(nestings);
    Expression_Obj disj1 = parse_disjunction();
    // if it's a singleton, return it (don't wrap it)
    if (peek_css< space_list_terminator >(position)
    ) {
      return disj1; }

    List_Obj space_list = SASS_MEMORY_NEW(List, pstate, 2, SASS_SPACE);
    space_list->append(disj1);

    while (
      !(peek_css< space_list_terminator >(position)) &&
      peek_css< optional_css_whitespace >() != end
    ) {
      // the space is parsed implicitly?
      space_list->append(parse_disjunction());
    }
    // return the list
    return space_list;
  }
  // EO parse_space_list

  // parse logical OR operation
  Expression_Obj Parser::parse_disjunction()
  {
    NESTING_GUARD(nestings);
    advanceToNextToken();
    ParserState state(pstate);
    // parse the left hand side conjunction
    Expression_Obj conj = parse_conjunction();
    // parse multiple right hand sides
    std::vector<Expression_Obj> operands;
    while (lex_css< kwd_or >())
      operands.push_back(parse_conjunction());
    // if it's a singleton, return it directly
    if (operands.size() == 0) return conj;
    // fold all operands into one binary expression
    Expression_Obj ex = fold_operands(conj, operands, { Sass_OP::OR });
    state.offset = pstate - state + pstate.offset;
    ex->pstate(state);
    return ex;
  }
  // EO parse_disjunction

  // parse logical AND operation
  Expression_Obj Parser::parse_conjunction()
  {
    NESTING_GUARD(nestings);
    advanceToNextToken();
    ParserState state(pstate);
    // parse the left hand side relation
    Expression_Obj rel = parse_relation();
    // parse multiple right hand sides
    std::vector<Expression_Obj> operands;
    while (lex_css< kwd_and >()) {
      operands.push_back(parse_relation());
    }
    // if it's a singleton, return it directly
    if (operands.size() == 0) return rel;
    // fold all operands into one binary expression
    Expression_Obj ex = fold_operands(rel, operands, { Sass_OP::AND });
    state.offset = pstate - state + pstate.offset;
    ex->pstate(state);
    return ex;
  }
  // EO parse_conjunction

  // parse comparison operations
  Expression_Obj Parser::parse_relation()
  {
    NESTING_GUARD(nestings);
    advanceToNextToken();
    ParserState state(pstate);
    // parse the left hand side expression
    Expression_Obj lhs = parse_expression();
    std::vector<Expression_Obj> operands;
    std::vector<Operand> operators;
    // if it's a singleton, return it (don't wrap it)
    while (peek< alternatives <
            kwd_eq,
            kwd_neq,
            kwd_gte,
            kwd_gt,
            kwd_lte,
            kwd_lt
          > >(position))
    {
      // is directly adjancent to expression?
      bool left_ws = peek < css_comments >() != NULL;
      // parse the operator
      enum Sass_OP op
      = lex<kwd_eq>()  ? Sass_OP::EQ
      : lex<kwd_neq>() ? Sass_OP::NEQ
      : lex<kwd_gte>() ? Sass_OP::GTE
      : lex<kwd_lte>() ? Sass_OP::LTE
      : lex<kwd_gt>()  ? Sass_OP::GT
      : lex<kwd_lt>()  ? Sass_OP::LT
      // we checked the possibilities on top of fn
      :                  Sass_OP::EQ;
      // is directly adjacent to expression?
      bool right_ws = peek < css_comments >() != NULL;
      operators.push_back({ op, left_ws, right_ws });
      operands.push_back(parse_expression());
    }
    // we are called recursively for list, so we first
    // fold inner binary expression which has delayed
    // correctly set to zero. After folding we also unwrap
    // single nested items. So we cannot set delay on the
    // returned result here, as we have lost nestings ...
    Expression_Obj ex = fold_operands(lhs, operands, operators);
    state.offset = pstate - state + pstate.offset;
    ex->pstate(state);
    return ex;
  }
  // parse_relation

  // parse expression valid for operations
  // called from parse_relation
  // called from parse_for_directive
  // called from parse_media_expression
  // parse addition and subtraction operations
  Expression_Obj Parser::parse_expression()
  {
    NESTING_GUARD(nestings);
    advanceToNextToken();
    ParserState state(pstate);
    // parses multiple add and subtract operations
    // NOTE: make sure that identifiers starting with
    // NOTE: dashes do NOT count as subtract operation
    Expression_Obj lhs = parse_operators();
    // if it's a singleton, return it (don't wrap it)
    if (!(peek_css< exactly<'+'> >(position) ||
          // condition is a bit mysterious, but some combinations should not be counted as operations
          (peek< no_spaces >(position) && peek< sequence< negate< unsigned_number >, exactly<'-'>, negate< space > > >(position)) ||
          (peek< sequence< negate< unsigned_number >, exactly<'-'>, negate< unsigned_number > > >(position))) ||
          peek< sequence < zero_plus < exactly <'-' > >, identifier > >(position))
    { return lhs; }

    std::vector<Expression_Obj> operands;
    std::vector<Operand> operators;
    bool left_ws = peek < css_comments >() != NULL;
    while (
      lex_css< exactly<'+'> >() ||

      (
      ! peek_css< sequence < zero_plus < exactly <'-' > >, identifier > >(position)
      && lex_css< sequence< negate< digit >, exactly<'-'> > >()
      )

    ) {

      bool right_ws = peek < css_comments >() != NULL;
      operators.push_back({ lexed.to_string() == "+" ? Sass_OP::ADD : Sass_OP::SUB, left_ws, right_ws });
      operands.push_back(parse_operators());
      left_ws = peek < css_comments >() != NULL;
    }

    if (operands.size() == 0) return lhs;
    Expression_Obj ex = fold_operands(lhs, operands, operators);
    state.offset = pstate - state + pstate.offset;
    ex->pstate(state);
    return ex;
  }

  // parse addition and subtraction operations
  Expression_Obj Parser::parse_operators()
  {
    NESTING_GUARD(nestings);
    advanceToNextToken();
    ParserState state(pstate);
    Expression_Obj factor = parse_factor();
    // if it's a singleton, return it (don't wrap it)
    std::vector<Expression_Obj> operands; // factors
    std::vector<Operand> operators; // ops
    // lex operations to apply to lhs
    const char* left_ws = peek < css_comments >();
    while (lex_css< class_char< static_ops > >()) {
      const char* right_ws = peek < css_comments >();
      switch(*lexed.begin) {
        case '*': operators.push_back({ Sass_OP::MUL, left_ws != 0, right_ws != 0 }); break;
        case '/': operators.push_back({ Sass_OP::DIV, left_ws != 0, right_ws != 0 }); break;
        case '%': operators.push_back({ Sass_OP::MOD, left_ws != 0, right_ws != 0 }); break;
        default: throw std::runtime_error("unknown static op parsed");
      }
      operands.push_back(parse_factor());
      left_ws = peek < css_comments >();
    }
    // operands and operators to binary expression
    Expression_Obj ex = fold_operands(factor, operands, operators);
    state.offset = pstate - state + pstate.offset;
    ex->pstate(state);
    return ex;
  }
  // EO parse_operators


  // called from parse_operators
  // called from parse_value_schema
  Expression_Obj Parser::parse_factor()
  {
    NESTING_GUARD(nestings);
    lex < css_comments >(false);
    if (lex_css< exactly<'('> >()) {
      // parse_map may return a list
      Expression_Obj value = parse_map();
      // lex the expected closing parenthesis
      if (!lex_css< exactly<')'> >()) error("unclosed parenthesis");
      // expression can be evaluated
      return value;
    }
    else if (lex_css< exactly<'['> >()) {
      // explicit bracketed
      Expression_Obj value = parse_bracket_list();
      // lex the expected closing square bracket
      if (!lex_css< exactly<']'> >()) error("unclosed squared bracket");
      return value;
    }
    // string may be interpolated
    // if (lex< quoted_string >()) {
    //   return &parse_string();
    // }
    else if (peek< ie_property >()) {
      return parse_ie_property();
    }
    else if (peek< ie_keyword_arg >()) {
      return parse_ie_keyword_arg();
    }
    else if (peek< sequence < calc_fn_call, exactly <'('> > >()) {
      return parse_calc_function();
    }
    else if (lex < functional_schema >()) {
      return parse_function_call_schema();
    }
    else if (lex< identifier_schema >()) {
      String_Obj string = parse_identifier_schema();
      if (String_Schema* schema = Cast<String_Schema>(string)) {
        if (lex < exactly < '(' > >()) {
          schema->append(parse_list());
          lex < exactly < ')' > >();
        }
      }
      return string;
    }
    else if (peek< sequence< uri_prefix, W, real_uri_value > >()) {
      return parse_url_function_string();
    }
    else if (peek< re_functional >()) {
      return parse_function_call();
    }
    else if (lex< exactly<'+'> >()) {
      Unary_Expression* ex = SASS_MEMORY_NEW(Unary_Expression, pstate, Unary_Expression::PLUS, parse_factor());
      if (ex && ex->operand()) ex->is_delayed(ex->operand()->is_delayed());
      return ex;
    }
    else if (lex< exactly<'-'> >()) {
      Unary_Expression* ex = SASS_MEMORY_NEW(Unary_Expression, pstate, Unary_Expression::MINUS, parse_factor());
      if (ex && ex->operand()) ex->is_delayed(ex->operand()->is_delayed());
      return ex;
    }
    else if (lex< exactly<'/'> >()) {
      Unary_Expression* ex = SASS_MEMORY_NEW(Unary_Expression, pstate, Unary_Expression::SLASH, parse_factor());
      if (ex && ex->operand()) ex->is_delayed(ex->operand()->is_delayed());
      return ex;
    }
    else if (lex< sequence< kwd_not > >()) {
      Unary_Expression* ex = SASS_MEMORY_NEW(Unary_Expression, pstate, Unary_Expression::NOT, parse_factor());
      if (ex && ex->operand()) ex->is_delayed(ex->operand()->is_delayed());
      return ex;
    }
    else {
      return parse_value();
    }
  }

  bool number_has_zero(const std::string& parsed)
  {
    size_t L = parsed.length();
    return !( (L > 0 && parsed.substr(0, 1) == ".") ||
              (L > 1 && parsed.substr(0, 2) == "0.") ||
              (L > 1 && parsed.substr(0, 2) == "-.")  ||
              (L > 2 && parsed.substr(0, 3) == "-0.") );
  }

  Number* Parser::lexed_number(const ParserState& pstate, const std::string& parsed)
  {
    Number* nr = SASS_MEMORY_NEW(Number,
                                    pstate,
                                    sass_strtod(parsed.c_str()),
                                    "",
                                    number_has_zero(parsed));
    nr->is_interpolant(false);
    nr->is_delayed(true);
    return nr;
  }

  Number* Parser::lexed_percentage(const ParserState& pstate, const std::string& parsed)
  {
    Number* nr = SASS_MEMORY_NEW(Number,
                                    pstate,
                                    sass_strtod(parsed.c_str()),
                                    "%",
                                    true);
    nr->is_interpolant(false);
    nr->is_delayed(true);
    return nr;
  }

  Number* Parser::lexed_dimension(const ParserState& pstate, const std::string& parsed)
  {
    size_t L = parsed.length();
    size_t num_pos = parsed.find_first_not_of(" \n\r\t");
    if (num_pos == std::string::npos) num_pos = L;
    size_t unit_pos = parsed.find_first_not_of("-+0123456789.", num_pos);
    if (parsed[unit_pos] == 'e' && is_number(parsed[unit_pos+1]) ) {
      unit_pos = parsed.find_first_not_of("-+0123456789.", ++ unit_pos);
    }
    if (unit_pos == std::string::npos) unit_pos = L;
    const std::string& num = parsed.substr(num_pos, unit_pos - num_pos);
    Number* nr = SASS_MEMORY_NEW(Number,
                                    pstate,
                                    sass_strtod(num.c_str()),
                                    Token(number(parsed.c_str())),
                                    number_has_zero(parsed));
    nr->is_interpolant(false);
    nr->is_delayed(true);
    return nr;
  }

  Value* Parser::lexed_hex_color(const ParserState& pstate, const std::string& parsed)
  {
    Color_RGBA* color = NULL;
    if (parsed[0] != '#') {
      return SASS_MEMORY_NEW(String_Quoted, pstate, parsed);
    }
    // chop off the '#'
    std::string hext(parsed.substr(1));
    if (parsed.length() == 4) {
      std::string r(2, parsed[1]);
      std::string g(2, parsed[2]);
      std::string b(2, parsed[3]);
      color = SASS_MEMORY_NEW(Color_RGBA,
                               pstate,
                               static_cast<double>(strtol(r.c_str(), NULL, 16)),
                               static_cast<double>(strtol(g.c_str(), NULL, 16)),
                               static_cast<double>(strtol(b.c_str(), NULL, 16)),
                               1, // alpha channel
                               parsed);
    }
    else if (parsed.length() == 5) {
      std::string r(2, parsed[1]);
      std::string g(2, parsed[2]);
      std::string b(2, parsed[3]);
      std::string a(2, parsed[4]);
      color = SASS_MEMORY_NEW(Color_RGBA,
                               pstate,
                               static_cast<double>(strtol(r.c_str(), NULL, 16)),
                               static_cast<double>(strtol(g.c_str(), NULL, 16)),
                               static_cast<double>(strtol(b.c_str(), NULL, 16)),
                               static_cast<double>(strtol(a.c_str(), NULL, 16)) / 255,
                               parsed);
    }
    else if (parsed.length() == 7) {
      std::string r(parsed.substr(1,2));
      std::string g(parsed.substr(3,2));
      std::string b(parsed.substr(5,2));
      color = SASS_MEMORY_NEW(Color_RGBA,
                               pstate,
                               static_cast<double>(strtol(r.c_str(), NULL, 16)),
                               static_cast<double>(strtol(g.c_str(), NULL, 16)),
                               static_cast<double>(strtol(b.c_str(), NULL, 16)),
                               1, // alpha channel
                               parsed);
    }
    else if (parsed.length() == 9) {
      std::string r(parsed.substr(1,2));
      std::string g(parsed.substr(3,2));
      std::string b(parsed.substr(5,2));
      std::string a(parsed.substr(7,2));
      color = SASS_MEMORY_NEW(Color_RGBA,
                               pstate,
                               static_cast<double>(strtol(r.c_str(), NULL, 16)),
                               static_cast<double>(strtol(g.c_str(), NULL, 16)),
                               static_cast<double>(strtol(b.c_str(), NULL, 16)),
                               static_cast<double>(strtol(a.c_str(), NULL, 16)) / 255,
                               parsed);
    }
    color->is_interpolant(false);
    color->is_delayed(false);
    return color;
  }

  Value* Parser::color_or_string(const std::string& lexed) const
  {
    if (auto color = name_to_color(lexed)) {
      auto c = SASS_MEMORY_NEW(Color_RGBA, color);
      c->is_delayed(true);
      c->pstate(pstate);
      c->disp(lexed);
      return c;
    } else {
      return SASS_MEMORY_NEW(String_Constant, pstate, lexed);
    }
  }

  // parse one value for a list
  Expression_Obj Parser::parse_value()
  {
    lex< css_comments >(false);
    if (lex< ampersand >())
    {
      if (match< ampersand >()) {
        warning("In Sass, \"&&\" means two copies of the parent selector. You probably want to use \"and\" instead.", pstate);
      }
      return SASS_MEMORY_NEW(Parent_Reference, pstate); }

    if (lex< kwd_important >())
    { return SASS_MEMORY_NEW(String_Constant, pstate, "!important"); }

    // parse `10%4px` into separated items and not a schema
    if (lex< sequence < percentage, lookahead < number > > >())
    { return lexed_percentage(lexed); }

    if (lex< sequence < number, lookahead< sequence < op, number > > > >())
    { return lexed_number(lexed); }

    // string may be interpolated
    if (lex< sequence < quoted_string, lookahead < exactly <'-'> > > >())
    { return parse_string(); }

    if (const char* stop = peek< value_schema >())
    { return parse_value_schema(stop); }

    // string may be interpolated
    if (lex< quoted_string >())
    { return parse_string(); }

    if (lex< kwd_true >())
    { return SASS_MEMORY_NEW(Boolean, pstate, true); }

    if (lex< kwd_false >())
    { return SASS_MEMORY_NEW(Boolean, pstate, false); }

    if (lex< kwd_null >())
    { return SASS_MEMORY_NEW(Null, pstate); }

    if (lex< identifier >()) {
      return color_or_string(lexed);
    }

    if (lex< percentage >())
    { return lexed_percentage(lexed); }

    // match hex number first because 0x000 looks like a number followed by an identifier
    if (lex< sequence < alternatives< hex, hex0 >, negate < exactly<'-'> > > >())
    { return lexed_hex_color(lexed); }

    if (lex< hexa >())
    { return lexed_hex_color(lexed); }

    if (lex< sequence < exactly <'#'>, identifier > >())
    { return SASS_MEMORY_NEW(String_Quoted, pstate, lexed); }

    // also handle the 10em- foo special case
    // alternatives < exactly < '.' >, .. > -- `1.5em-.75em` is split into a list, not a binary expression
    if (lex< sequence< dimension, optional< sequence< exactly<'-'>, lookahead< alternatives < space > > > > > >())
    { return lexed_dimension(lexed); }

    if (lex< sequence< static_component, one_plus< strict_identifier > > >())
    { return SASS_MEMORY_NEW(String_Constant, pstate, lexed); }

    if (lex< number >())
    { return lexed_number(lexed); }

    if (lex< variable >())
    { return SASS_MEMORY_NEW(Variable, pstate, Util::normalize_underscores(lexed)); }

    css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was ");

    // unreachable statement
    return {};
  }

  // this parses interpolation inside other strings
  // means the result should later be quoted again
  String_Obj Parser::parse_interpolated_chunk(Token chunk, bool constant, bool css)
  {
    const char* i = chunk.begin;
    // see if there any interpolants
    const char* p = constant ? find_first_in_interval< exactly<hash_lbrace> >(i, chunk.end) :
                    find_first_in_interval< exactly<hash_lbrace>, block_comment >(i, chunk.end);

    if (!p) {
      String_Quoted* str_quoted = SASS_MEMORY_NEW(String_Quoted, pstate, std::string(i, chunk.end), 0, false, false, true, css);
      if (!constant && str_quoted->quote_mark()) str_quoted->quote_mark('*');
      return str_quoted;
    }

    String_Schema_Obj schema = SASS_MEMORY_NEW(String_Schema, pstate, 0, css);
    schema->is_interpolant(true);
    while (i < chunk.end) {
      p = constant ? find_first_in_interval< exactly<hash_lbrace> >(i, chunk.end) :
          find_first_in_interval< exactly<hash_lbrace>, block_comment >(i, chunk.end);
      if (p) {
        if (i < p) {
          // accumulate the preceding segment if it's nonempty
          schema->append(SASS_MEMORY_NEW(String_Constant, pstate, std::string(i, p), css));
        }
        // we need to skip anything inside strings
        // create a new target in parser/prelexer
        if (peek < sequence < optional_spaces, exactly<rbrace> > >(p+2)) { position = p+2;
          css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was ");
        }
        const char* j = skip_over_scopes< exactly<hash_lbrace>, exactly<rbrace> >(p + 2, chunk.end); // find the closing brace
        if (j) { --j;
          // parse the interpolant and accumulate it
          Expression_Obj interp_node = Parser::from_token(Token(p+2, j), ctx, traces, pstate, source).parse_list();
          interp_node->is_interpolant(true);
          schema->append(interp_node);
          i = j;
        }
        else {
          // throw an error if the interpolant is unterminated
          error("unterminated interpolant inside string constant " + chunk.to_string());
        }
      }
      else { // no interpolants left; add the last segment if nonempty
        // check if we need quotes here (was not sure after merge)
        if (i < chunk.end) schema->append(SASS_MEMORY_NEW(String_Constant, pstate, std::string(i, chunk.end), css));
        break;
      }
      ++ i;
    }

    return schema.detach();
  }

  String_Schema_Obj Parser::parse_css_variable_value()
  {
    String_Schema_Obj schema = SASS_MEMORY_NEW(String_Schema, pstate);
    std::vector<char> brackets;
    while (true) {
      if (
        (brackets.empty() && lex< css_variable_top_level_value >(false)) ||
        (!brackets.empty() && lex< css_variable_value >(false))
      ) {
        Token str(lexed);
        schema->append(SASS_MEMORY_NEW(String_Constant, pstate, str));
      } else if (Expression_Obj tok = lex_interpolation()) {
        if (String_Schema* s = Cast<String_Schema>(tok)) {
          if (s->empty()) break;
          schema->concat(s);
        } else {
          schema->append(tok);
        }
      } else if (lex< quoted_string >()) {
        Expression_Obj tok = parse_string();
        if (tok.isNull()) break;
        if (String_Schema* s = Cast<String_Schema>(tok)) {
          if (s->empty()) break;
          schema->concat(s);
        } else {
          schema->append(tok);
        }
      } else if (lex< alternatives< exactly<'('>, exactly<'['>, exactly<'{'> > >()) {
        const char opening_bracket = *(position - 1);
        brackets.push_back(opening_bracket);
        schema->append(SASS_MEMORY_NEW(String_Constant, pstate, std::string(1, opening_bracket)));
      } else if (const char *match = peek< alternatives< exactly<')'>, exactly<']'>, exactly<'}'> > >()) {
        if (brackets.empty()) break;
        const char closing_bracket = *(match - 1);
        if (brackets.back() != Util::opening_bracket_for(closing_bracket)) {
          std::string message = ": expected \"";
          message += Util::closing_bracket_for(brackets.back());
          message += "\", was ";
          css_error("Invalid CSS", " after ", message);
        }
        lex< alternatives< exactly<')'>, exactly<']'>, exactly<'}'> > >();
        schema->append(SASS_MEMORY_NEW(String_Constant, pstate, std::string(1, closing_bracket)));
        brackets.pop_back();
      } else {
        break;
      }
    }

    if (!brackets.empty()) {
      std::string message = ": expected \"";
      message += Util::closing_bracket_for(brackets.back());
      message += "\", was ";
      css_error("Invalid CSS", " after ", message);
    }

    if (schema->empty()) error("Custom property values may not be empty.");
    return schema.detach();
  }

  Value_Obj Parser::parse_static_value()
  {
    lex< static_value >();
    Token str(lexed);
    // static values always have trailing white-
    // space and end delimiter (\s*[;]$) included
    --pstate.offset.column;
    --after_token.column;
    --str.end;
    --position;

    return color_or_string(str.time_wspace());;
  }

  String_Obj Parser::parse_string()
  {
    return parse_interpolated_chunk(Token(lexed));
  }

  String_Obj Parser::parse_ie_property()
  {
    lex< ie_property >();
    Token str(lexed);
    const char* i = str.begin;
    // see if there any interpolants
    const char* p = find_first_in_interval< exactly<hash_lbrace>, block_comment >(str.begin, str.end);
    if (!p) {
      return SASS_MEMORY_NEW(String_Quoted, pstate, std::string(str.begin, str.end));
    }

    String_Schema* schema = SASS_MEMORY_NEW(String_Schema, pstate);
    while (i < str.end) {
      p = find_first_in_interval< exactly<hash_lbrace>, block_comment >(i, str.end);
      if (p) {
        if (i < p) {
          schema->append(SASS_MEMORY_NEW(String_Constant, pstate, std::string(i, p))); // accumulate the preceding segment if it's nonempty
        }
        if (peek < sequence < optional_spaces, exactly<rbrace> > >(p+2)) { position = p+2;
          css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was ");
        }
        const char* j = skip_over_scopes< exactly<hash_lbrace>, exactly<rbrace> >(p+2, str.end); // find the closing brace
        if (j) {
          // parse the interpolant and accumulate it
          Expression_Obj interp_node = Parser::from_token(Token(p+2, j), ctx, traces, pstate, source).parse_list();
          interp_node->is_interpolant(true);
          schema->append(interp_node);
          i = j;
        }
        else {
          // throw an error if the interpolant is unterminated
          error("unterminated interpolant inside IE function " + str.to_string());
        }
      }
      else { // no interpolants left; add the last segment if nonempty
        if (i < str.end) {
          schema->append(SASS_MEMORY_NEW(String_Constant, pstate, std::string(i, str.end)));
        }
        break;
      }
    }
    return schema;
  }

  String_Obj Parser::parse_ie_keyword_arg()
  {
    String_Schema_Obj kwd_arg = SASS_MEMORY_NEW(String_Schema, pstate, 3);
    if (lex< variable >()) {
      kwd_arg->append(SASS_MEMORY_NEW(Variable, pstate, Util::normalize_underscores(lexed)));
    } else {
      lex< alternatives< identifier_schema, identifier > >();
      kwd_arg->append(SASS_MEMORY_NEW(String_Constant, pstate, lexed));
    }
    lex< exactly<'='> >();
    kwd_arg->append(SASS_MEMORY_NEW(String_Constant, pstate, lexed));
    if (peek< variable >()) kwd_arg->append(parse_list());
    else if (lex< number >()) {
      std::string parsed(lexed);
      Util::normalize_decimals(parsed);
      kwd_arg->append(lexed_number(parsed));
    }
    else if (peek < ie_keyword_arg_value >()) { kwd_arg->append(parse_list()); }
    return kwd_arg;
  }

  String_Schema_Obj Parser::parse_value_schema(const char* stop)
  {
    // initialize the string schema object to add tokens
    String_Schema_Obj schema = SASS_MEMORY_NEW(String_Schema, pstate);

    if (peek<exactly<'}'>>()) {
      css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was ");
    }

    const char* e;
    const char* ee = end;
    end = stop;
    size_t num_items = 0;
    bool need_space = false;
    while (position < stop) {
      // parse space between tokens
      if (lex< spaces >() && num_items) {
        need_space = true;
      }
      if (need_space) {
        need_space = false;
        // schema->append(SASS_MEMORY_NEW(String_Constant, pstate, " "));
      }
      if ((e = peek< re_functional >()) && e < stop) {
        schema->append(parse_function_call());
      }
      // lex an interpolant /#{...}/
      else if (lex< exactly < hash_lbrace > >()) {
        // Try to lex static expression first
        if (peek< exactly< rbrace > >()) {
          css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was ");
        }
        Expression_Obj ex;
        if (lex< re_static_expression >()) {
          ex = SASS_MEMORY_NEW(String_Constant, pstate, lexed);
        } else {
          ex = parse_list(true);
        }
        ex->is_interpolant(true);
        schema->append(ex);
        if (!lex < exactly < rbrace > >()) {
          css_error("Invalid CSS", " after ", ": expected \"}\", was ");
        }
      }
      // lex some string constants or other valid token
      // Note: [-+] chars are left over from i.e. `#{3}+3`
      else if (lex< alternatives < exactly<'%'>, exactly < '-' >, exactly < '+' > > >()) {
        schema->append(SASS_MEMORY_NEW(String_Constant, pstate, lexed));
      }
      // lex a quoted string
      else if (lex< quoted_string >()) {
        // need_space = true;
        // if (schema->length()) schema->append(SASS_MEMORY_NEW(String_Constant, pstate, " "));
        // else need_space = true;
        schema->append(parse_string());
        if ((*position == '"' || *position == '\'') || peek < alternatives < alpha > >()) {
          // need_space = true;
        }
        if (peek < exactly < '-' > >()) break;
      }
      else if (lex< identifier >()) {
        schema->append(SASS_MEMORY_NEW(String_Constant, pstate, lexed));
        if ((*position == '"' || *position == '\'') || peek < alternatives < alpha > >()) {
           // need_space = true;
        }
      }
      // lex (normalized) variable
      else if (lex< variable >()) {
        std::string name(Util::normalize_underscores(lexed));
        schema->append(SASS_MEMORY_NEW(Variable, pstate, name));
      }
      // lex percentage value
      else if (lex< percentage >()) {
        schema->append(lexed_percentage(lexed));
      }
      // lex dimension value
      else if (lex< dimension >()) {
        schema->append(lexed_dimension(lexed));
      }
      // lex number value
      else if (lex< number >()) {
        schema->append(lexed_number(lexed));
      }
      // lex hex color value
      else if (lex< sequence < hex, negate < exactly < '-' > > > >()) {
        schema->append(lexed_hex_color(lexed));
      }
      else if (lex< sequence < exactly <'#'>, identifier > >()) {
        schema->append(SASS_MEMORY_NEW(String_Quoted, pstate, lexed));
      }
      // lex a value in parentheses
      else if (peek< parenthese_scope >()) {
        schema->append(parse_factor());
      }
      else {
        break;
      }
      ++num_items;
    }
    if (position != stop) {
      schema->append(SASS_MEMORY_NEW(String_Constant, pstate, std::string(position, stop)));
      position = stop;
    }
    end = ee;
    return schema;
  }

  // this parses interpolation outside other strings
  // means the result must not be quoted again later
  String_Obj Parser::parse_identifier_schema()
  {
    Token id(lexed);
    const char* i = id.begin;
    // see if there any interpolants
    const char* p = find_first_in_interval< exactly<hash_lbrace>, block_comment >(id.begin, id.end);
    if (!p) {
      return SASS_MEMORY_NEW(String_Constant, pstate, std::string(id.begin, id.end));
    }

    String_Schema_Obj schema = SASS_MEMORY_NEW(String_Schema, pstate);
    while (i < id.end) {
      p = find_first_in_interval< exactly<hash_lbrace>, block_comment >(i, id.end);
      if (p) {
        if (i < p) {
          // accumulate the preceding segment if it's nonempty
          const char* o = position; position = i;
          schema->append(parse_value_schema(p));
          position = o;
        }
        // we need to skip anything inside strings
        // create a new target in parser/prelexer
        if (peek < sequence < optional_spaces, exactly<rbrace> > >(p+2)) { position = p;
          css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was ");
        }
        const char* j = skip_over_scopes< exactly<hash_lbrace>, exactly<rbrace> >(p+2, id.end); // find the closing brace
        if (j) {
          // parse the interpolant and accumulate it
          Expression_Obj interp_node = Parser::from_token(Token(p+2, j), ctx, traces, pstate, source).parse_list(DELAYED);
          interp_node->is_interpolant(true);
          schema->append(interp_node);
          // schema->has_interpolants(true);
          i = j;
        }
        else {
          // throw an error if the interpolant is unterminated
          error("unterminated interpolant inside interpolated identifier " + id.to_string());
        }
      }
      else { // no interpolants left; add the last segment if nonempty
        if (i < end) {
          const char* o = position; position = i;
          schema->append(parse_value_schema(id.end));
          position = o;
        }
        break;
      }
    }
    return schema ? schema.detach() : 0;
  }

  // calc functions should preserve arguments
  Function_Call_Obj Parser::parse_calc_function()
  {
    lex< identifier >();
    std::string name(lexed);
    ParserState call_pos = pstate;
    lex< exactly<'('> >();
    ParserState arg_pos = pstate;
    const char* arg_beg = position;
    parse_list();
    const char* arg_end = position;
    lex< skip_over_scopes <
          exactly < '(' >,
          exactly < ')' >
        > >();

    Argument_Obj arg = SASS_MEMORY_NEW(Argument, arg_pos, parse_interpolated_chunk(Token(arg_beg, arg_end)));
    Arguments_Obj args = SASS_MEMORY_NEW(Arguments, arg_pos);
    args->append(arg);
    return SASS_MEMORY_NEW(Function_Call, call_pos, name, args);
  }

  String_Obj Parser::parse_url_function_string()
  {
    std::string prefix("");
    if (lex< uri_prefix >()) {
      prefix = std::string(lexed);
    }

    lex < optional_spaces >();
    String_Obj url_string = parse_url_function_argument();

    std::string suffix("");
    if (lex< real_uri_suffix >()) {
      suffix = std::string(lexed);
    }

    std::string uri("");
    if (url_string) {
      uri = url_string->to_string({ NESTED, 5 });
    }

    if (String_Schema* schema = Cast<String_Schema>(url_string)) {
      String_Schema_Obj res = SASS_MEMORY_NEW(String_Schema, pstate);
      res->append(SASS_MEMORY_NEW(String_Constant, pstate, prefix));
      res->append(schema);
      res->append(SASS_MEMORY_NEW(String_Constant, pstate, suffix));
      return res;
    } else {
      std::string res = prefix + uri + suffix;
      return SASS_MEMORY_NEW(String_Constant, pstate, res);
    }
  }

  String_Obj Parser::parse_url_function_argument()
  {
    const char* p = position;

    std::string uri("");
    if (lex< real_uri_value >(false)) {
      uri = lexed.to_string();
    }

    if (peek< exactly< hash_lbrace > >()) {
      const char* pp = position;
      // TODO: error checking for unclosed interpolants
      while (pp && peek< exactly< hash_lbrace > >(pp)) {
        pp = sequence< interpolant, real_uri_value >(pp);
      }
      if (!pp) return {};
      position = pp;
      return parse_interpolated_chunk(Token(p, position));
    }
    else if (uri != "") {
      std::string res = Util::rtrim(uri);
      return SASS_MEMORY_NEW(String_Constant, pstate, res);
    }

    return {};
  }

  Function_Call_Obj Parser::parse_function_call()
  {
    lex< identifier >();
    std::string name(lexed);

    if (Util::normalize_underscores(name) == "content-exists" && stack.back() != Scope::Mixin)
    { error("Cannot call content-exists() except within a mixin."); }

    ParserState call_pos = pstate;
    Arguments_Obj args = parse_arguments();
    return SASS_MEMORY_NEW(Function_Call, call_pos, name, args);
  }

  Function_Call_Obj Parser::parse_function_call_schema()
  {
    String_Obj name = parse_identifier_schema();
    ParserState source_position_of_call = pstate;
    Arguments_Obj args = parse_arguments();

    return SASS_MEMORY_NEW(Function_Call, source_position_of_call, name, args);
  }

  Content_Obj Parser::parse_content_directive()
  {
    ParserState call_pos = pstate;
    Arguments_Obj args = parse_arguments();

    return SASS_MEMORY_NEW(Content, call_pos, args);
  }

  If_Obj Parser::parse_if_directive(bool else_if)
  {
    stack.push_back(Scope::Control);
    ParserState if_source_position = pstate;
    bool root = block_stack.back()->is_root();
    Expression_Obj predicate = parse_list();
    Block_Obj block = parse_block(root);
    Block_Obj alternative;

    // only throw away comment if we parse a case
    // we want all other comments to be parsed
    if (lex_css< elseif_directive >()) {
      alternative = SASS_MEMORY_NEW(Block, pstate);
      alternative->append(parse_if_directive(true));
    }
    else if (lex_css< kwd_else_directive >()) {
      alternative = parse_block(root);
    }
    stack.pop_back();
    return SASS_MEMORY_NEW(If, if_source_position, predicate, block, alternative);
  }

  For_Obj Parser::parse_for_directive()
  {
    stack.push_back(Scope::Control);
    ParserState for_source_position = pstate;
    bool root = block_stack.back()->is_root();
    lex_variable();
    std::string var(Util::normalize_underscores(lexed));
    if (!lex< kwd_from >()) error("expected 'from' keyword in @for directive");
    Expression_Obj lower_bound = parse_expression();
    bool inclusive = false;
    if (lex< kwd_through >()) inclusive = true;
    else if (lex< kwd_to >()) inclusive = false;
    else                  error("expected 'through' or 'to' keyword in @for directive");
    Expression_Obj upper_bound = parse_expression();
    Block_Obj body = parse_block(root);
    stack.pop_back();
    return SASS_MEMORY_NEW(For, for_source_position, var, lower_bound, upper_bound, body, inclusive);
  }

  // helper to parse a var token
  Token Parser::lex_variable()
  {
    // peek for dollar sign first
    if (!peek< exactly <'$'> >()) {
      css_error("Invalid CSS", " after ", ": expected \"$\", was ");
    }
    // we expect a simple identifier as the call name
    if (!lex< sequence < exactly <'$'>, identifier > >()) {
      lex< exactly <'$'> >(); // move pstate and position up
      css_error("Invalid CSS", " after ", ": expected identifier, was ");
    }
    // return object
    return token;
  }
  // helper to parse identifier
  Token Parser::lex_identifier()
  {
    // we expect a simple identifier as the call name
    if (!lex< identifier >()) { // ToDo: pstate wrong?
      css_error("Invalid CSS", " after ", ": expected identifier, was ");
    }
    // return object
    return token;
  }

  Each_Obj Parser::parse_each_directive()
  {
    stack.push_back(Scope::Control);
    ParserState each_source_position = pstate;
    bool root = block_stack.back()->is_root();
    std::vector<std::string> vars;
    lex_variable();
    vars.push_back(Util::normalize_underscores(lexed));
    while (lex< exactly<','> >()) {
      if (!lex< variable >()) error("@each directive requires an iteration variable");
      vars.push_back(Util::normalize_underscores(lexed));
    }
    if (!lex< kwd_in >()) error("expected 'in' keyword in @each directive");
    Expression_Obj list = parse_list();
    Block_Obj body = parse_block(root);
    stack.pop_back();
    return SASS_MEMORY_NEW(Each, each_source_position, vars, list, body);
  }

  // called after parsing `kwd_while_directive`
  While_Obj Parser::parse_while_directive()
  {
    stack.push_back(Scope::Control);
    bool root = block_stack.back()->is_root();
    // create the initial while call object
    While_Obj call = SASS_MEMORY_NEW(While, pstate, {}, {});
    // parse mandatory predicate
    Expression_Obj predicate = parse_list();
    List_Obj l = Cast<List>(predicate);
    if (!predicate || (l && !l->length())) {
      css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was ", false);
    }
    call->predicate(predicate);
    // parse mandatory block
    call->block(parse_block(root));
    // return ast node
    stack.pop_back();
    // return ast node
    return call.detach();
  }


  std::vector<CssMediaQuery_Obj> Parser::parseCssMediaQueries()
  {
    std::vector<CssMediaQuery_Obj> result;
    do {
      if (auto query = parseCssMediaQuery()) {
        result.push_back(query);
      }
    } while (lex<exactly<','>>());
    return result;
  }

  std::string Parser::parseIdentifier()
  {
    if (lex < identifier >(false)) {
      return std::string(lexed);
    }
    return std::string();
  }

  CssMediaQuery_Obj Parser::parseCssMediaQuery()
  {
    CssMediaQuery_Obj result = SASS_MEMORY_NEW(CssMediaQuery, pstate);
    lex<css_comments>(false);

    // Check if any tokens are to parse
    if (!peek_css<exactly<'('>>()) {

      std::string token1(parseIdentifier());
      lex<css_comments>(false);

      if (token1.empty()) {
        return {};
      }

      std::string token2(parseIdentifier());
      lex<css_comments>(false);

      if (Util::equalsLiteral("and", token2)) {
        result->type(token1);
      }
      else {
        if (token2.empty()) {
          result->type(token1);
        }
        else {
          result->modifier(token1);
          result->type(token2);
        }

        if (lex < kwd_and >()) {
          lex<css_comments>(false);
        }
        else {
          return result;
        }

      }

    }

    std::vector<std::string> queries;

    do {
      lex<css_comments>(false);

      if (lex<exactly<'('>>()) {
        // In dart sass parser returns a pure string
        if (lex < skip_over_scopes < exactly < '(' >, exactly < ')' > > >()) {
          std::string decl("(" + std::string(lexed));
          queries.push_back(decl);
        }
        // Should be: parseDeclarationValue;
        if (!lex<exactly<')'>>()) {
          // Should we throw an error here?
        }
      }
    } while (lex < kwd_and >());

    result->features(queries);

    if (result->features().empty()) {
      if (result->type().empty()) {
        return {};
      }
    }

    return result;
  }


  // EO parse_while_directive
  MediaRule_Obj Parser::parseMediaRule()
  {
    MediaRule_Obj rule = SASS_MEMORY_NEW(MediaRule, pstate);
    stack.push_back(Scope::Media);
    rule->schema(parse_media_queries());
    parse_block_comments(false);
    rule->block(parse_css_block());
    stack.pop_back();
    return rule;
  }

  List_Obj Parser::parse_media_queries()
  {
    advanceToNextToken();
    List_Obj queries = SASS_MEMORY_NEW(List, pstate, 0, SASS_COMMA);
    if (!peek_css < exactly <'{'> >()) queries->append(parse_media_query());
    while (lex_css < exactly <','> >()) queries->append(parse_media_query());
    queries->update_pstate(pstate);
    return queries.detach();
  }

  // Expression* Parser::parse_media_query()
  Media_Query_Obj Parser::parse_media_query()
  {
    advanceToNextToken();
    Media_Query_Obj media_query = SASS_MEMORY_NEW(Media_Query, pstate);
    if (lex < kwd_not >()) { media_query->is_negated(true); lex < css_comments >(false); }
    else if (lex < kwd_only >()) { media_query->is_restricted(true); lex < css_comments >(false); }

    if (lex < identifier_schema >()) media_query->media_type(parse_identifier_schema());
    else if (lex < identifier >())   media_query->media_type(parse_interpolated_chunk(lexed));
    else                             media_query->append(parse_media_expression());

    while (lex_css < kwd_and >()) media_query->append(parse_media_expression());
    if (lex < identifier_schema >()) {
      String_Schema* schema = SASS_MEMORY_NEW(String_Schema, pstate);
      if (media_query->media_type()) {
        schema->append(media_query->media_type());
        schema->append(SASS_MEMORY_NEW(String_Constant, pstate, " "));
      }
      schema->append(parse_identifier_schema());
      media_query->media_type(schema);
    }
    while (lex_css < kwd_and >()) media_query->append(parse_media_expression());

    media_query->update_pstate(pstate);

    return media_query;
  }

  Media_Query_Expression_Obj Parser::parse_media_expression()
  {
    if (lex < identifier_schema >()) {
      String_Obj ss = parse_identifier_schema();
      return SASS_MEMORY_NEW(Media_Query_Expression, pstate, ss, {}, true);
    }
    if (!lex_css< exactly<'('> >()) {
      error("media query expression must begin with '('");
    }
    Expression_Obj feature;
    if (peek_css< exactly<')'> >()) {
      error("media feature required in media query expression");
    }
    feature = parse_expression();
    Expression_Obj expression;
    if (lex_css< exactly<':'> >()) {
      expression = parse_list(DELAYED);
    }
    if (!lex_css< exactly<')'> >()) {
      error("unclosed parenthesis in media query expression");
    }
    return SASS_MEMORY_NEW(Media_Query_Expression, feature->pstate(), feature, expression);
  }

  // lexed after `kwd_supports_directive`
  // these are very similar to media blocks
  Supports_Block_Obj Parser::parse_supports_directive()
  {
    Supports_Condition_Obj cond = parse_supports_condition(/*top_level=*/true);
    // create the ast node object for the support queries
    Supports_Block_Obj query = SASS_MEMORY_NEW(Supports_Block, pstate, cond);
    // additional block is mandatory
    // parse inner block
    query->block(parse_block());
    // return ast node
    return query;
  }

  // parse one query operation
  // may encounter nested queries
  Supports_Condition_Obj Parser::parse_supports_condition(bool top_level)
  {
    lex < css_whitespace >();
    Supports_Condition_Obj cond;
    if ((cond = parse_supports_negation())) return cond;
    if ((cond = parse_supports_operator(top_level))) return cond;
    if ((cond = parse_supports_interpolation())) return cond;
    return cond;
  }

  Supports_Condition_Obj Parser::parse_supports_negation()
  {
    if (!lex < kwd_not >()) return {};
    Supports_Condition_Obj cond = parse_supports_condition_in_parens(/*parens_required=*/true);
    return SASS_MEMORY_NEW(Supports_Negation, pstate, cond);
  }

  Supports_Condition_Obj Parser::parse_supports_operator(bool top_level)
  {
    Supports_Condition_Obj cond = parse_supports_condition_in_parens(/*parens_required=*/top_level);
    if (cond.isNull()) return {};

    while (true) {
      Supports_Operator::Operand op = Supports_Operator::OR;
      if (lex < kwd_and >()) { op = Supports_Operator::AND; }
      else if(!lex < kwd_or >()) { break; }

      lex < css_whitespace >();
      Supports_Condition_Obj right = parse_supports_condition_in_parens(/*parens_required=*/true);

      // Supports_Condition* cc = SASS_MEMORY_NEW(Supports_Condition, *static_cast<Supports_Condition*>(cond));
      cond = SASS_MEMORY_NEW(Supports_Operator, pstate, cond, right, op);
    }
    return cond;
  }

  Supports_Condition_Obj Parser::parse_supports_interpolation()
  {
    if (!lex < interpolant >()) return {};

    String_Obj interp = parse_interpolated_chunk(lexed);
    if (!interp) return {};

    return SASS_MEMORY_NEW(Supports_Interpolation, pstate, interp);
  }

  // TODO: This needs some major work. Although feature conditions
  // look like declarations their semantics differ significantly
  Supports_Condition_Obj Parser::parse_supports_declaration()
  {
    Supports_Condition* cond;
    // parse something declaration like
    Expression_Obj feature = parse_expression();
    Expression_Obj expression;
    if (lex_css< exactly<':'> >()) {
      expression = parse_list(DELAYED);
    }
    if (!feature || !expression) error("@supports condition expected declaration");
    cond = SASS_MEMORY_NEW(Supports_Declaration,
                     feature->pstate(),
                     feature,
                     expression);
    // ToDo: maybe we need an additional error condition?
    return cond;
  }

  Supports_Condition_Obj Parser::parse_supports_condition_in_parens(bool parens_required)
  {
    Supports_Condition_Obj interp = parse_supports_interpolation();
    if (interp != nullptr) return interp;

    if (!lex < exactly <'('> >()) {
      if (parens_required) {
        css_error("Invalid CSS", " after ", ": expected @supports condition (e.g. (display: flexbox)), was ", /*trim=*/false);
      } else {
        return {};
      }
    }
    lex < css_whitespace >();

    Supports_Condition_Obj cond = parse_supports_condition(/*top_level=*/false);
    if (cond.isNull()) cond = parse_supports_declaration();
    if (!lex < exactly <')'> >()) error("unclosed parenthesis in @supports declaration");

    lex < css_whitespace >();
    return cond;
  }

  At_Root_Block_Obj Parser::parse_at_root_block()
  {
    stack.push_back(Scope::AtRoot);
    ParserState at_source_position = pstate;
    Block_Obj body;
    At_Root_Query_Obj expr;
    Lookahead lookahead_result;
    if (lex_css< exactly<'('> >()) {
      expr = parse_at_root_query();
    }
    if (peek_css < exactly<'{'> >()) {
      lex <optional_spaces>();
      body = parse_block(true);
    }
    else if ((lookahead_result = lookahead_for_selector(position)).found) {
      Ruleset_Obj r = parse_ruleset(lookahead_result);
      body = SASS_MEMORY_NEW(Block, r->pstate(), 1, true);
      body->append(r);
    }
    At_Root_Block_Obj at_root = SASS_MEMORY_NEW(At_Root_Block, at_source_position, body);
    if (!expr.isNull()) at_root->expression(expr);
    stack.pop_back();
    return at_root;
  }

  At_Root_Query_Obj Parser::parse_at_root_query()
  {
    if (peek< exactly<')'> >()) error("at-root feature required in at-root expression");

    if (!peek< alternatives< kwd_with_directive, kwd_without_directive > >()) {
      css_error("Invalid CSS", " after ", ": expected \"with\" or \"without\", was ");
    }

    Expression_Obj feature = parse_list();
    if (!lex_css< exactly<':'> >()) error("style declaration must contain a value");
    Expression_Obj expression = parse_list();
    List_Obj value = SASS_MEMORY_NEW(List, feature->pstate(), 1);

    if (expression->concrete_type() == Expression::LIST) {
        value = Cast<List>(expression);
    }
    else value->append(expression);

    At_Root_Query_Obj cond = SASS_MEMORY_NEW(At_Root_Query,
                                          value->pstate(),
                                          feature,
                                          value);
    if (!lex_css< exactly<')'> >()) error("unclosed parenthesis in @at-root expression");
    return cond;
  }

  Directive_Obj Parser::parse_directive()
  {
    Directive_Obj directive = SASS_MEMORY_NEW(Directive, pstate, lexed);
    String_Schema_Obj val = parse_almost_any_value();
    // strip left and right if they are of type string
    directive->value(val);
    if (peek< exactly<'{'> >()) {
      directive->block(parse_block());
    }
    return directive;
  }

  Expression_Obj Parser::lex_interpolation()
  {
    if (lex < interpolant >(true) != NULL) {
      return parse_interpolated_chunk(lexed, true);
    }
    return {};
  }

  Expression_Obj Parser::lex_interp_uri()
  {
    // create a string schema by lexing optional interpolations
    return lex_interp< re_string_uri_open, re_string_uri_close >();
  }

  Expression_Obj Parser::lex_interp_string()
  {
    Expression_Obj rv;
    if ((rv = lex_interp< re_string_double_open, re_string_double_close >())) return rv;
    if ((rv = lex_interp< re_string_single_open, re_string_single_close >())) return rv;
    return rv;
  }

  Expression_Obj Parser::lex_almost_any_value_chars()
  {
    const char* match =
    lex <
      one_plus <
        alternatives <
          exactly <'>'>,
          sequence <
            exactly <'\\'>,
            any_char
          >,
          sequence <
            negate <
              sequence <
                exactly < url_kwd >,
                exactly <'('>
              >
            >,
            neg_class_char <
              almost_any_value_class
            >
          >,
          sequence <
            exactly <'/'>,
            negate <
              alternatives <
                exactly <'/'>,
                exactly <'*'>
              >
            >
          >,
          sequence <
            exactly <'\\'>,
            exactly <'#'>,
            negate <
              exactly <'{'>
            >
          >,
          sequence <
            exactly <'!'>,
            negate <
              alpha
            >
          >
        >
      >
    >(false);
    if (match) {
      return SASS_MEMORY_NEW(String_Constant, pstate, lexed);
    }
    return {};
  }

  Expression_Obj Parser::lex_almost_any_value_token()
  {
    Expression_Obj rv;
    if (*position == 0) return {};
    if ((rv = lex_almost_any_value_chars())) return rv;
    // if ((rv = lex_block_comment())) return rv;
    // if ((rv = lex_single_line_comment())) return rv;
    if ((rv = lex_interp_string())) return rv;
    if ((rv = lex_interp_uri())) return rv;
    if ((rv = lex_interpolation())) return rv;
     if (lex< alternatives< hex, hex0 > >())
    { return lexed_hex_color(lexed); }
   return rv;
  }

  String_Schema_Obj Parser::parse_almost_any_value()
  {

    String_Schema_Obj schema = SASS_MEMORY_NEW(String_Schema, pstate);
    if (*position == 0) return {};
    lex < spaces >(false);
    Expression_Obj token = lex_almost_any_value_token();
    if (!token) return {};
    schema->append(token);
    if (*position == 0) {
      schema->rtrim();
      return schema.detach();
    }

    while ((token = lex_almost_any_value_token())) {
      schema->append(token);
    }

    lex < css_whitespace >();

    schema->rtrim();

    return schema.detach();
  }

  Warning_Obj Parser::parse_warning()
  {
    if (stack.back() != Scope::Root &&
        stack.back() != Scope::Function &&
        stack.back() != Scope::Mixin &&
        stack.back() != Scope::Control &&
        stack.back() != Scope::Rules) {
      error("Illegal nesting: Only properties may be nested beneath properties.");
    }
    return SASS_MEMORY_NEW(Warning, pstate, parse_list(DELAYED));
  }

  Error_Obj Parser::parse_error()
  {
    if (stack.back() != Scope::Root &&
        stack.back() != Scope::Function &&
        stack.back() != Scope::Mixin &&
        stack.back() != Scope::Control &&
        stack.back() != Scope::Rules) {
      error("Illegal nesting: Only properties may be nested beneath properties.");
    }
    return SASS_MEMORY_NEW(Error, pstate, parse_list(DELAYED));
  }

  Debug_Obj Parser::parse_debug()
  {
    if (stack.back() != Scope::Root &&
        stack.back() != Scope::Function &&
        stack.back() != Scope::Mixin &&
        stack.back() != Scope::Control &&
        stack.back() != Scope::Rules) {
      error("Illegal nesting: Only properties may be nested beneath properties.");
    }
    return SASS_MEMORY_NEW(Debug, pstate, parse_list(DELAYED));
  }

  Return_Obj Parser::parse_return_directive()
  {
    // check that we do not have an empty list (ToDo: check if we got all cases)
    if (peek_css < alternatives < exactly < ';' >, exactly < '}' >, end_of_file > >())
    { css_error("Invalid CSS", " after ", ": expected expression (e.g. 1px, bold), was "); }
    return SASS_MEMORY_NEW(Return, pstate, parse_list());
  }

  Lookahead Parser::lookahead_for_selector(const char* start)
  {
    // init result struct
    Lookahead rv = Lookahead();
    // get start position
    const char* p = start ? start : position;
    // match in one big "regex"
    rv.error = p;
    if (const char* q =
      peek <
        re_selector_list
      >(p)
    ) {
      bool could_be_property = peek< sequence< exactly<'-'>, exactly<'-'> > >(p) != 0;
      bool could_be_escaped = false;
      while (p < q) {
        // did we have interpolations?
        if (*p == '#' && *(p+1) == '{') {
          rv.has_interpolants = true;
          p = q; break;
        }
        // A property that's ambiguous with a nested selector is interpreted as a
        // custom property.
        if (*p == ':' && !could_be_escaped) {
          rv.is_custom_property = could_be_property || p+1 == q || peek< space >(p+1);
        }
        could_be_escaped = *p == '\\';
        ++ p;
      }
      // store anyway  }


      // ToDo: remove
      rv.error = q;
      rv.position = q;
      // check expected opening bracket
      // only after successful matching
      if (peek < exactly<'{'> >(q)) rv.found = q;
      // else if (peek < end_of_file >(q)) rv.found = q;
      else if (peek < exactly<'('> >(q)) rv.found = q;
      // else if (peek < exactly<';'> >(q)) rv.found = q;
      // else if (peek < exactly<'}'> >(q)) rv.found = q;
      if (rv.found || *p == 0) rv.error = 0;
    }

    rv.parsable = ! rv.has_interpolants;

    // return result
    return rv;

  }
  // EO lookahead_for_selector

  // used in parse_block_nodes and parse_special_directive
  // ToDo: actual usage is still not really clear to me?
  Lookahead Parser::lookahead_for_include(const char* start)
  {
    // we actually just lookahead for a selector
    Lookahead rv = lookahead_for_selector(start);
    // but the "found" rules are different
    if (const char* p = rv.position) {
      // check for additional abort condition
      if (peek < exactly<';'> >(p)) rv.found = p;
      else if (peek < exactly<'}'> >(p)) rv.found = p;
    }
    // return result
    return rv;
  }
  // EO lookahead_for_include

  // look ahead for a token with interpolation in it
  // we mostly use the result if there is an interpolation
  // everything that passes here gets parsed as one schema
  // meaning it will not be parsed as a space separated list
  Lookahead Parser::lookahead_for_value(const char* start)
  {
    // init result struct
    Lookahead rv = Lookahead();
    // get start position
    const char* p = start ? start : position;
    // match in one big "regex"
    if (const char* q =
      peek <
        non_greedy <
          alternatives <
            // consume whitespace
            block_comment, // spaces,
            // main tokens
            sequence <
              interpolant,
              optional <
                quoted_string
              >
            >,
            identifier,
            variable,
            // issue #442
            sequence <
              parenthese_scope,
              interpolant,
              optional <
                quoted_string
              >
            >
          >,
          sequence <
            // optional_spaces,
            alternatives <
              // end_of_file,
              exactly<'{'>,
              exactly<'}'>,
              exactly<';'>
            >
          >
        >
      >(p)
    ) {
      if (p == q) return rv;
      while (p < q) {
        // did we have interpolations?
        if (*p == '#' && *(p+1) == '{') {
          rv.has_interpolants = true;
          p = q; break;
        }
        ++ p;
      }
      // store anyway
      // ToDo: remove
      rv.position = q;
      // check expected opening bracket
      // only after successful matching
      if (peek < exactly<'{'> >(q)) rv.found = q;
      else if (peek < exactly<';'> >(q)) rv.found = q;
      else if (peek < exactly<'}'> >(q)) rv.found = q;
    }

    // return result
    return rv;
  }
  // EO lookahead_for_value

  void Parser::read_bom()
  {
    size_t skip = 0;
    std::string encoding;
    bool utf_8 = false;
    switch ((unsigned char) source[0]) {
    case 0xEF:
      skip = check_bom_chars(source, end, utf_8_bom, 3);
      encoding = "UTF-8";
      utf_8 = true;
      break;
    case 0xFE:
      skip = check_bom_chars(source, end, utf_16_bom_be, 2);
      encoding = "UTF-16 (big endian)";
      break;
    case 0xFF:
      skip = check_bom_chars(source, end, utf_16_bom_le, 2);
      skip += (skip ? check_bom_chars(source, end, utf_32_bom_le, 4) : 0);
      encoding = (skip == 2 ? "UTF-16 (little endian)" : "UTF-32 (little endian)");
      break;
    case 0x00:
      skip = check_bom_chars(source, end, utf_32_bom_be, 4);
      encoding = "UTF-32 (big endian)";
      break;
    case 0x2B:
      skip = check_bom_chars(source, end, utf_7_bom_1, 4)
           | check_bom_chars(source, end, utf_7_bom_2, 4)
           | check_bom_chars(source, end, utf_7_bom_3, 4)
           | check_bom_chars(source, end, utf_7_bom_4, 4)
           | check_bom_chars(source, end, utf_7_bom_5, 5);
      encoding = "UTF-7";
      break;
    case 0xF7:
      skip = check_bom_chars(source, end, utf_1_bom, 3);
      encoding = "UTF-1";
      break;
    case 0xDD:
      skip = check_bom_chars(source, end, utf_ebcdic_bom, 4);
      encoding = "UTF-EBCDIC";
      break;
    case 0x0E:
      skip = check_bom_chars(source, end, scsu_bom, 3);
      encoding = "SCSU";
      break;
    case 0xFB:
      skip = check_bom_chars(source, end, bocu_1_bom, 3);
      encoding = "BOCU-1";
      break;
    case 0x84:
      skip = check_bom_chars(source, end, gb_18030_bom, 4);
      encoding = "GB-18030";
      break;
    default: break;
    }
    if (skip > 0 && !utf_8) error("only UTF-8 documents are currently supported; your document appears to be " + encoding);
    position += skip;
  }

  size_t check_bom_chars(const char* src, const char *end, const unsigned char* bom, size_t len)
  {
    size_t skip = 0;
    if (src + len > end) return 0;
    for (size_t i = 0; i < len; ++i, ++skip) {
      if ((unsigned char) src[i] != bom[i]) return 0;
    }
    return skip;
  }


  Expression_Obj Parser::fold_operands(Expression_Obj base, std::vector<Expression_Obj>& operands, Operand op)
  {
    for (size_t i = 0, S = operands.size(); i < S; ++i) {
      base = SASS_MEMORY_NEW(Binary_Expression, base->pstate(), op, base, operands[i]);
    }
    return base;
  }

  Expression_Obj Parser::fold_operands(Expression_Obj base, std::vector<Expression_Obj>& operands, std::vector<Operand>& ops, size_t i)
  {
    if (String_Schema* schema = Cast<String_Schema>(base)) {
      // return schema;
      if (schema->has_interpolants()) {
        if (i + 1 < operands.size() && (
             (ops[0].operand == Sass_OP::EQ)
          || (ops[0].operand == Sass_OP::ADD)
          || (ops[0].operand == Sass_OP::DIV)
          || (ops[0].operand == Sass_OP::MUL)
          || (ops[0].operand == Sass_OP::NEQ)
          || (ops[0].operand == Sass_OP::LT)
          || (ops[0].operand == Sass_OP::GT)
          || (ops[0].operand == Sass_OP::LTE)
          || (ops[0].operand == Sass_OP::GTE)
        )) {
          Expression_Obj rhs = fold_operands(operands[i], operands, ops, i + 1);
          rhs = SASS_MEMORY_NEW(Binary_Expression, base->pstate(), ops[0], schema, rhs);
          return rhs;
        }
        // return schema;
      }
    }

    for (size_t S = operands.size(); i < S; ++i) {
      if (String_Schema* schema = Cast<String_Schema>(operands[i])) {
        if (schema->has_interpolants()) {
          if (i + 1 < S) {
            // this whole branch is never hit via spec tests
            Expression_Obj rhs = fold_operands(operands[i+1], operands, ops, i + 2);
            rhs = SASS_MEMORY_NEW(Binary_Expression, base->pstate(), ops[i], schema, rhs);
            base = SASS_MEMORY_NEW(Binary_Expression, base->pstate(), ops[i], base, rhs);
            return base;
          }
          base = SASS_MEMORY_NEW(Binary_Expression, base->pstate(), ops[i], base, operands[i]);
          return base;
        } else {
          base = SASS_MEMORY_NEW(Binary_Expression, base->pstate(), ops[i], base, operands[i]);
        }
      } else {
        base = SASS_MEMORY_NEW(Binary_Expression, base->pstate(), ops[i], base, operands[i]);
      }
      Binary_Expression* b = Cast<Binary_Expression>(base.ptr());
      if (b && ops[i].operand == Sass_OP::DIV && b->left()->is_delayed() && b->right()->is_delayed()) {
        base->is_delayed(true);
      }
    }
    // nested binary expression are never to be delayed
    if (Binary_Expression* b = Cast<Binary_Expression>(base)) {
      if (Cast<Binary_Expression>(b->left())) base->set_delayed(false);
      if (Cast<Binary_Expression>(b->right())) base->set_delayed(false);
    }
    return base;
  }

  void Parser::error(std::string msg, Position pos)
  {
    Position p(pos.line ? pos : before_token);
    ParserState pstate(path, source, p, Offset(0, 0));
    // `pstate.src` may not outlive stack unwind so we must copy it.
    // This is needed since we often parse dynamically generated code,
    // e.g. for interpolations, and we normally don't want to keep this
    // memory around after we parsed the AST tree successfully. Only on
    // errors we want to preserve them for better error reporting.
    char *src_copy = sass_copy_c_string(pstate.src);
    pstate.src = src_copy;
    traces.push_back(Backtrace(pstate));
    throw Exception::InvalidSass(pstate, traces, msg, src_copy);
  }

  void Parser::error(std::string msg)
  {
    error(msg, pstate);
  }

  // print a css parsing error with actual context information from parsed source
  void Parser::css_error(const std::string& msg, const std::string& prefix, const std::string& middle, const bool trim)
  {
    int max_len = 18;
    const char* end = this->end;
    while (*end != 0) ++ end;
    const char* pos = peek < optional_spaces >();
    if (!pos) pos = position;

    const char* last_pos(pos);
    if (last_pos > source) {
      utf8::prior(last_pos, source);
    }
    // backup position to last significant char
    while (trim && last_pos > source && last_pos < end) {
      if (!Util::ascii_isspace(static_cast<unsigned char>(*last_pos))) break;
      utf8::prior(last_pos, source);
    }

    bool ellipsis_left = false;
    const char* pos_left(last_pos);
    const char* end_left(last_pos);

    if (*pos_left) utf8::next(pos_left, end);
    if (*end_left) utf8::next(end_left, end);
    while (pos_left > source) {
      if (utf8::distance(pos_left, end_left) >= max_len) {
        utf8::prior(pos_left, source);
        ellipsis_left = *(pos_left) != '\n' &&
                        *(pos_left) != '\r';
        utf8::next(pos_left, end);
        break;
      }

      const char* prev = pos_left;
      utf8::prior(prev, source);
      if (*prev == '\r') break;
      if (*prev == '\n') break;
      pos_left = prev;
    }
    if (pos_left < source) {
      pos_left = source;
    }

    bool ellipsis_right = false;
    const char* end_right(pos);
    const char* pos_right(pos);
    while (end_right < end) {
      if (utf8::distance(pos_right, end_right) > max_len) {
        ellipsis_left = *(pos_right) != '\n' &&
                        *(pos_right) != '\r';
        break;
      }
      if (*end_right == '\r') break;
      if (*end_right == '\n') break;
      utf8::next(end_right, end);
    }
    // if (*end_right == 0) end_right ++;

    std::string left(pos_left, end_left);
    std::string right(pos_right, end_right);
    size_t left_subpos = left.size() > 15 ? left.size() - 15 : 0;
    size_t right_subpos = right.size() > 15 ? right.size() - 15 : 0;
    if (left_subpos && ellipsis_left) left = ellipsis + left.substr(left_subpos);
    if (right_subpos && ellipsis_right) right = right.substr(right_subpos) + ellipsis;
    // Hotfix when source is null, probably due to interpolation parsing!?
    if (source == NULL || *source == 0) source = pstate.src;
    // now pass new message to the more generic error function
    error(msg + prefix + quote(left) + middle + quote(right));
  }

}
