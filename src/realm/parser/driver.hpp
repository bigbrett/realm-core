#ifndef DRIVER_HH
#define DRIVER_HH
#include <string>
#include <map>

#include "realm/query_expression.hpp"
#include "realm/parser/keypath_mapping.hpp"
#include "realm/parser/query_parser.hpp"

#define YY_DECL yy::parser::symbol_type yylex(void* yyscanner)
#include "realm/parser/generated/query_bison.hpp"
YY_DECL;

#undef FALSE
#undef TRUE
#undef IN

namespace realm {

namespace query_parser {

class ParserNode {
public:
    virtual ~ParserNode();
};

/******************************** Query Nodes ********************************/

class QueryNode : public ParserNode {
public:
    ~QueryNode() override;
    virtual Query visit(ParserDriver*) = 0;
    virtual void canonicalize() {}
};

class TrueOrFalseNode : public QueryNode {
public:
    TrueOrFalseNode(bool type)
        : true_or_false(type)
    {
    }
    Query visit(ParserDriver*);

protected:
    bool true_or_false;
};

class LogicalNode : public QueryNode {
public:
    std::vector<QueryNode*> children;
    LogicalNode(QueryNode* left, QueryNode* right)
    {
        children.emplace_back(left);
        children.emplace_back(right);
    }
    void canonicalize() override
    {
        std::vector<LogicalNode*> todo;
        do_canonicalize(todo);
        while (todo.size()) {
            LogicalNode* cur = todo.back();
            todo.pop_back();
            cur->do_canonicalize(todo);
        }
    }

    void do_canonicalize(std::vector<LogicalNode*>& todo)
    {
        auto& my_type = typeid(*this);
        size_t index = 0;
        while (index < children.size()) {
            QueryNode* child = *(children.begin() + index);
            auto& child_type = typeid(*child);
            if (child_type == my_type) {
                auto logical_node = static_cast<LogicalNode*>(child);
                REALM_ASSERT_EX(logical_node->children.size() == 2, logical_node->children.size());
                children.push_back(logical_node->children[0]);
                children.push_back(logical_node->children[1]);
                children.erase(children.begin() + index);
                continue; // do not ++index because of the delete
            }
            else if (auto ln = dynamic_cast<LogicalNode*>(child)) {
                todo.push_back(ln);
            }
            else {
                child->canonicalize();
            }
            ++index;
        }
    }

private:
    virtual std::string get_operator() const = 0;
};

class AndNode : public LogicalNode {
public:
    using LogicalNode::LogicalNode;
    Query visit(ParserDriver*) override;

private:
    std::string get_operator() const override
    {
        return " && ";
    }
};

class OrNode : public LogicalNode {
public:
    using LogicalNode::LogicalNode;
    Query visit(ParserDriver*) override;

private:
    std::string get_operator() const override
    {
        return " || ";
    }
};

class NotNode : public QueryNode {
public:
    QueryNode* query = nullptr;

    NotNode(QueryNode* q)
        : query(q)
    {
    }
    Query visit(ParserDriver*) override;
};

/****************************** Expression Nodes *****************************/

class ExpressionNode : public ParserNode {
public:
    virtual bool is_constant()
    {
        return false;
    }
    virtual bool is_list()
    {
        return false;
    }
    virtual std::unique_ptr<Subexpr> visit(ParserDriver*, DataType = type_Int) = 0;
};

/******************************** Value Nodes ********************************/

class ValueNode : public ExpressionNode {
};

class ConstantNode : public ValueNode {
public:
    enum Type {
        NUMBER,
        INFINITY_VAL,
        NAN_VAL,
        FLOAT,
        STRING,
        BASE64,
        TIMESTAMP,
        UUID_T,
        OID,
        LINK,
        TYPED_LINK,
        NULL_VAL,
        TRUE,
        FALSE,
        ARG
    };

    Type type;
    std::string text;


    ConstantNode(Type t, const std::string& str)
        : type(t)
        , text(str)
    {
    }
    ConstantNode(ExpressionComparisonType comp_type, const std::string& str)
        : type(Type::ARG)
        , text(str)
        , m_comp_type(comp_type)
    {
    }
    bool is_constant() final
    {
        return true;
    }
    void add_table(std::string table_name)
    {
        target_table = table_name.substr(1, table_name.size() - 2);
    }
    std::unique_ptr<Subexpr> visit(ParserDriver*, DataType) override;
    util::Optional<ExpressionComparisonType> m_comp_type;
    std::string target_table;
};

class ListNode : public ValueNode {
public:
    std::vector<ConstantNode*> elements;

    ListNode() = default;
    ListNode(ConstantNode* elem)
    {
        elements.emplace_back(elem);
    }
    bool is_constant() final
    {
        return true;
    }
    bool is_list() final
    {
        return true;
    }
    void add_element(ConstantNode* elem)
    {
        elements.emplace_back(elem);
    }
    void set_comp_type(ExpressionComparisonType comp_type)
    {
        m_comp_type = comp_type;
    }
    std::unique_ptr<Subexpr> visit(ParserDriver*, DataType);

private:
    util::Optional<ExpressionComparisonType> m_comp_type;
};

class PathNode : public ParserNode {
public:
    std::vector<PathElem> path_elems;

    PathNode(PathElem first)
    {
        add_element(first);
    }
    LinkChain visit(ParserDriver*, util::Optional<ExpressionComparisonType> = util::none);
    void add_element(const PathElem& elem)
    {
        if (backlink) {
            path_elems.back().id = path_elems.back().id + "." + elem.id;
            if (backlink == 2) {
                backlink = 0;
            }
            else {
                backlink++;
            }
        }
        else {
            if (elem.id == "@links")
                backlink = 1;
            path_elems.push_back(elem);
        }
    }

private:
    int backlink = 0;
};

class PropertyNode : public ValueNode {
public:
    PathNode* path;
    util::Optional<ExpressionComparisonType> comp_type = util::none;
    PostOpNode* post_op = nullptr;

    PropertyNode(PathNode* path, util::Optional<ExpressionComparisonType> ct = util::none)
        : path(path)
        , comp_type(ct)
    {
    }
    const std::string& identifier() const
    {
        return path->path_elems.back().id;
    }
    const LinkChain& link_chain() const
    {
        return m_link_chain;
    }
    void add_postop(PostOpNode* po)
    {
        post_op = po;
    }
    std::unique_ptr<Subexpr> visit(ParserDriver*, DataType = type_Int) override;

private:
    LinkChain m_link_chain;
};

class AggrNode : public ValueNode {
public:
    enum Type { MAX, MIN, SUM, AVG };

protected:
    PropertyNode* property;
    Type type;

    AggrNode(PropertyNode* node, int t)
        : property(node)
        , type(Type(t))
    {
    }
    std::unique_ptr<Subexpr> aggregate(Subexpr*);
};

class ListAggrNode : public AggrNode {
public:
    ListAggrNode(PropertyNode* node, int t)
        : AggrNode(node, t)
    {
    }

protected:
    std::unique_ptr<Subexpr> visit(ParserDriver*, DataType) override;
};

class LinkAggrNode : public AggrNode {
public:
    LinkAggrNode(PropertyNode* node, int t, std::string id)
        : AggrNode(node, t)
        , prop_name(id)
    {
    }

protected:
    std::string prop_name;
    std::unique_ptr<Subexpr> visit(ParserDriver*, DataType) override;
};

class SubqueryNode : public ValueNode {
public:
    PropertyNode* prop = nullptr;
    std::string variable_name;
    QueryNode* subquery = nullptr;

    SubqueryNode(PropertyNode* node, std::string var_name, QueryNode* query)
        : prop(node)
        , variable_name(var_name)
        , subquery(query)
    {
    }
    std::unique_ptr<Subexpr> visit(ParserDriver*, DataType) override;
};

class OperationNode : public ExpressionNode {
public:
    ExpressionNode* m_left;
    ExpressionNode* m_right;
    char m_op;
    OperationNode(ExpressionNode* left, char op, ExpressionNode* right)
        : m_left(left)
        , m_right(right)
        , m_op(op)
    {
    }
    bool is_constant() final
    {
        return m_left->is_constant() && m_right->is_constant();
    }
    bool is_list() final
    {
        return m_left->is_list() || m_right->is_list();
    }
    std::unique_ptr<Subexpr> visit(ParserDriver*, DataType) override;
};

/******************************* Compare Nodes *******************************/

class CompareNode : public QueryNode {
public:
    static constexpr int EQUAL = 0;
    static constexpr int NOT_EQUAL = 1;
    static constexpr int GREATER = 2;
    static constexpr int LESS = 3;
    static constexpr int GREATER_EQUAL = 4;
    static constexpr int LESS_EQUAL = 5;
    static constexpr int BEGINSWITH = 6;
    static constexpr int ENDSWITH = 7;
    static constexpr int CONTAINS = 8;
    static constexpr int LIKE = 9;
    static constexpr int IN = 10;
    static constexpr int TEXT = 11;
};

class EqualityNode : public CompareNode {
public:
    std::vector<ExpressionNode*> values;
    int op;
    bool case_sensitive = true;

    EqualityNode(ExpressionNode* left, int t, ExpressionNode* right)
        : op(t)
    {
        values.emplace_back(left);
        values.emplace_back(right);
    }
    Query visit(ParserDriver*) override;
};

class RelationalNode : public CompareNode {
public:
    std::vector<ExpressionNode*> values;
    int op;

    RelationalNode(ExpressionNode* left, int t, ExpressionNode* right)
        : op(t)
    {
        values.emplace_back(left);
        values.emplace_back(right);
    }
    Query visit(ParserDriver*) override;
};

class BetweenNode : public CompareNode {
public:
    ValueNode* prop;
    ListNode* limits;

    BetweenNode(ValueNode* left, ListNode* right)
        : prop(left)
        , limits(right)
    {
    }
    Query visit(ParserDriver*) override;
};

class StringOpsNode : public CompareNode {
public:
    std::vector<ExpressionNode*> values;
    int op;
    bool case_sensitive = true;

    StringOpsNode(ValueNode* left, int t, ValueNode* right)
        : op(t)
    {
        values.emplace_back(left);
        values.emplace_back(right);
    }
    Query visit(ParserDriver*) override;
};

/******************************** Other Nodes ********************************/

class PostOpNode : public ParserNode {
public:
    enum OpType { SIZE, TYPE } op_type;
    std::string op_name;

    PostOpNode(std::string op_literal, OpType type)
        : op_type(type)
        , op_name(op_literal)
    {
    }
    std::unique_ptr<Subexpr> visit(ParserDriver*, Subexpr* subexpr);
};


class DescriptorNode : public ParserNode {
public:
    enum Type { SORT, DISTINCT, LIMIT };
    std::vector<std::vector<std::string>> columns;
    std::vector<bool> ascending;
    size_t limit = size_t(-1);
    Type type;

    DescriptorNode(Type t)
        : type(t)
    {
    }
    DescriptorNode(Type t, const std::string& str)
        : type(t)
    {
        limit = size_t(strtol(str.c_str(), nullptr, 0));
    }
    ~DescriptorNode() override;
    Type get_type()
    {
        return type;
    }
    void add(PathNode* path)
    {
        auto& vec = columns.emplace_back();
        for (PathElem& e : path->path_elems) {
            vec.push_back(e.id);
        }
    }
    void add(PathNode* path, bool direction)
    {
        add(path);
        ascending.push_back(direction);
    }
};

class DescriptorOrderingNode : public ParserNode {
public:
    std::vector<DescriptorNode*> orderings;

    DescriptorOrderingNode() = default;
    ~DescriptorOrderingNode() override;
    void add_descriptor(DescriptorNode* n)
    {
        orderings.push_back(n);
    }
    std::unique_ptr<DescriptorOrdering> visit(ParserDriver* drv);
};

// Conducting the whole scanning and parsing of Calc++.
class ParserDriver {
public:
    using SubexprPtr = std::unique_ptr<Subexpr>;
    class ParserNodeStore {
    public:
        template <typename T, typename... Args>
        T* create(Args&&... args)
        {
            auto owned = std::make_unique<T>(std::forward<Args>(args)...);
            auto ret = owned.get();
            m_store.push_back(std::move(owned));
            return ret;
        }

    private:
        std::vector<std::unique_ptr<ParserNode>> m_store;
    };

    ParserDriver()
        : ParserDriver(TableRef(), s_default_args, s_default_mapping)
    {
    }

    ParserDriver(TableRef t, Arguments& args, const query_parser::KeyPathMapping& mapping);
    ~ParserDriver();

    util::serializer::SerialisationState m_serializer_state;
    QueryNode* result = nullptr;
    DescriptorOrderingNode* ordering = nullptr;
    TableRef m_base_table;
    Arguments& m_args;
    query_parser::KeyPathMapping m_mapping;
    ParserNodeStore m_parse_nodes;
    void* m_yyscanner;

    // Run the parser on file F.  Return 0 on success.
    int parse(const std::string& str);

    // Handling the scanner.
    void scan_begin(void*, bool trace_scanning);

    void error(const std::string& err)
    {
        error_string = err;
        parse_error = true;
    }

    Mixed get_arg_for_index(std::string);

    template <class T>
    Query simple_query(int op, ColKey col_key, T val, bool case_sensitive);
    template <class T>
    Query simple_query(int op, ColKey col_key, T val);
    std::pair<SubexprPtr, SubexprPtr> cmp(const std::vector<ExpressionNode*>& values);
    SubexprPtr column(LinkChain&, std::string);
    void backlink(LinkChain&, const std::string&);
    std::string translate(const LinkChain&, const std::string&);

private:
    // The string being parsed.
    std::string parse_buffer;
    std::string error_string;
    void* scan_buffer = nullptr;
    bool parse_error = false;

    static NoArguments s_default_args;
    static query_parser::KeyPathMapping s_default_mapping;
};

template <class T>
Query ParserDriver::simple_query(int op, ColKey col_key, T val, bool case_sensitive)
{
    switch (op) {
        case CompareNode::IN:
        case CompareNode::EQUAL:
            return m_base_table->where().equal(col_key, val, case_sensitive);
        case CompareNode::NOT_EQUAL:
            return m_base_table->where().not_equal(col_key, val, case_sensitive);
    }
    return m_base_table->where();
}

template <class T>
Query ParserDriver::simple_query(int op, ColKey col_key, T val)
{
    switch (op) {
        case CompareNode::IN:
        case CompareNode::EQUAL:
            return m_base_table->where().equal(col_key, val);
        case CompareNode::NOT_EQUAL:
            return m_base_table->where().not_equal(col_key, val);
        case CompareNode::GREATER:
            return m_base_table->where().greater(col_key, val);
        case CompareNode::LESS:
            return m_base_table->where().less(col_key, val);
        case CompareNode::GREATER_EQUAL:
            return m_base_table->where().greater_equal(col_key, val);
        case CompareNode::LESS_EQUAL:
            return m_base_table->where().less_equal(col_key, val);
    }
    return m_base_table->where();
}

std::string check_escapes(const char* str);

} // namespace query_parser
} // namespace realm
#endif // ! DRIVER_HH
