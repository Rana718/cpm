#pragma once

#include "db.hpp"
#include <seastar/http/httpd.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/routes.hh>
#include <seastar/core/coroutine.hh>

namespace httpd = seastar::httpd;
namespace ss = seastar;

class GetCountries : public httpd::handler_base {
    CountryDB& db_;
public:
    GetCountries(CountryDB& db) : db_(db) {}
    ss::future<std::unique_ptr<httpd::reply>> handle(
        const ss::sstring&, std::unique_ptr<httpd::request>,
        std::unique_ptr<httpd::reply> rep) override {
        rep->set_content_type("json");
        rep->_content = db_.list_all().dump(2);
        rep->done("json");
        co_return std::move(rep);
    }
};

class GetCountryById : public httpd::handler_base {
    CountryDB& db_;
public:
    GetCountryById(CountryDB& db) : db_(db) {}
    ss::future<std::unique_ptr<httpd::reply>> handle(
        const ss::sstring&, std::unique_ptr<httpd::request> req,
        std::unique_ptr<httpd::reply> rep) override {
        rep->set_content_type("json");
        try {
            int id = std::stoi(req->param["id"].c_str());
            auto c = db_.get_by_id(id);
            if (c) { rep->_content = country_to_json(*c).dump(2); }
            else { rep->set_status(httpd::reply::status_type::not_found);
                   rep->_content = json{{"error","not found"}}.dump(); }
        } catch (...) {
            rep->set_status(httpd::reply::status_type::bad_request);
            rep->_content = json{{"error","invalid id"}}.dump();
        }
        rep->done("json");
        co_return std::move(rep);
    }
};

class GetCountryByCode : public httpd::handler_base {
    CountryDB& db_;
public:
    GetCountryByCode(CountryDB& db) : db_(db) {}
    ss::future<std::unique_ptr<httpd::reply>> handle(
        const ss::sstring&, std::unique_ptr<httpd::request> req,
        std::unique_ptr<httpd::reply> rep) override {
        rep->set_content_type("json");
        std::string code = req->param["code"].c_str();
        auto c = db_.get_by_code(code);
        if (c) { rep->_content = country_to_json(*c).dump(2); }
        else { rep->set_status(httpd::reply::status_type::not_found);
               rep->_content = json{{"error","not found"}}.dump(); }
        rep->done("json");
        co_return std::move(rep);
    }
};

class PostCountry : public httpd::handler_base {
    CountryDB& db_;
public:
    PostCountry(CountryDB& db) : db_(db) {}
    ss::future<std::unique_ptr<httpd::reply>> handle(
        const ss::sstring&, std::unique_ptr<httpd::request> req,
        std::unique_ptr<httpd::reply> rep) override {
        rep->set_content_type("json");
        json body;
        try { body = json::parse(req->content); }
        catch (...) {
            rep->set_status(httpd::reply::status_type::bad_request);
            rep->_content = json{{"error","invalid JSON"}}.dump();
            rep->done("json"); co_return std::move(rep);
        }
        Country c;
        auto err = validate_country(body, c);
        if (!err.empty()) {
            rep->set_status(httpd::reply::status_type::bad_request);
            rep->_content = json{{"error", err}}.dump();
        } else {
            auto [ok, msg] = db_.add(std::move(c));
            if (!ok) { rep->set_status(httpd::reply::status_type::bad_request);
                       rep->_content = json{{"error", msg}}.dump(); }
            else { rep->_content = db_.last().dump(2); }
        }
        rep->done("json");
        co_return std::move(rep);
    }
};

class PostEcho : public httpd::handler_base {
public:
    ss::future<std::unique_ptr<httpd::reply>> handle(
        const ss::sstring&, std::unique_ptr<httpd::request> req,
        std::unique_ptr<httpd::reply> rep) override {
        rep->set_content_type("json");
        rep->_content = req->content;
        rep->done("json");
        co_return std::move(rep);
    }
};
