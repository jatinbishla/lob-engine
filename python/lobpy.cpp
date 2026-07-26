#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstring>
#include <string>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/itch_parser.hpp"

namespace py = pybind11;
using namespace lob;

PYBIND11_MODULE(lobpy, m) {
    m.doc() = "LOB Matching Engine — Python bindings";

    py::enum_<Side>(m, "Side")
        .value("Buy",  Side::Buy)
        .value("Sell", Side::Sell);

    py::enum_<OrderType>(m, "OrderType")
        .value("Limit",  OrderType::Limit)
        .value("Market", OrderType::Market);

    py::enum_<RiskResult>(m, "RiskResult")
        .value("OK",              RiskResult::OK)
        .value("REJECT_SIZE",     RiskResult::REJECT_SIZE)
        .value("REJECT_POSITION", RiskResult::REJECT_POSITION)
        .value("REJECT_NOTIONAL", RiskResult::REJECT_NOTIONAL);

    py::class_<Order>(m, "Order")
        .def(py::init([](OrderId id, Side side, OrderType type,
                         Price price, Quantity qty) {
                 return Order{id, side, type, price, qty};
             }),
             py::arg("id"), py::arg("side"),
             py::arg("type") = OrderType::Limit,
             py::arg("price") = 0, py::arg("quantity") = 0)
        .def_readwrite("id",       &Order::id)
        .def_readwrite("side",     &Order::side)
        .def_readwrite("type",     &Order::type)
        .def_readwrite("price",    &Order::price)
        .def_readwrite("quantity", &Order::quantity);

    py::class_<Trade>(m, "Trade")
        .def_readonly("taker_id", &Trade::taker_id)
        .def_readonly("maker_id", &Trade::maker_id)
        .def_readonly("price",    &Trade::price)
        .def_readonly("quantity", &Trade::quantity)
        .def_readonly("seq",      &Trade::seq)
        .def("__repr__", [](const Trade& t) {
            return "<Trade maker=" + std::to_string(t.maker_id) +
                   " taker="       + std::to_string(t.taker_id) +
                   " qty="         + std::to_string(t.quantity) +
                   " px="          + std::to_string(t.price) + ">";
        });

    py::class_<OrderBook::SubmitResult>(m, "SubmitResult")
        .def_readonly("trades", &OrderBook::SubmitResult::trades)
        .def_readonly("risk",   &OrderBook::SubmitResult::risk);

    py::class_<OrderBook>(m, "OrderBook")
        .def(py::init<>())
        .def("submit", &OrderBook::submit, py::arg("order"))
        .def("cancel", &OrderBook::cancel, py::arg("id"))
        .def("rest",    &OrderBook::rest,    py::arg("order"))
        .def("reduce",  &OrderBook::reduce,  py::arg("id"), py::arg("qty"))
        .def("replace", &OrderBook::replace,
             py::arg("old_id"), py::arg("new_id"),
             py::arg("new_price"), py::arg("new_qty"))
        .def("best_bid", [](const OrderBook& b) -> py::object {
            Price p; return b.best_bid(p) ? py::cast(p) : py::none();
        })
        .def("best_ask", [](const OrderBook& b) -> py::object {
            Price p; return b.best_ask(p) ? py::cast(p) : py::none();
        })
        .def("depth_at", &OrderBook::depth_at,
             py::arg("side"), py::arg("price"));

    py::class_<itch::ParseStats>(m, "ParseStats")
        .def_readonly("messages_processed", &itch::ParseStats::messages_processed)
        .def_readonly("add_orders",         &itch::ParseStats::add_orders)
        .def_readonly("executions",         &itch::ParseStats::executions)
        .def_readonly("cancels",            &itch::ParseStats::cancels)
        .def_readonly("deletes",            &itch::ParseStats::deletes)
        .def_readonly("replaces",           &itch::ParseStats::replaces)
        .def_readonly("skipped",            &itch::ParseStats::skipped)
        .def_readonly("malformed",          &itch::ParseStats::malformed);

    py::class_<itch::ItchHandler>(m, "ItchHandler")
        .def(py::init<OrderBook&>(), py::keep_alive<1, 2>(), py::arg("book"))
        // Accept a Python bytes/bytearray of a length-prefixed ITCH 5.0 stream.
        .def("process_stream", [](itch::ItchHandler& h, py::bytes data) {
            std::string s = data;  // owns a copy for the duration of the call
            auto p = reinterpret_cast<const std::uint8_t*>(s.data());
            return h.process_stream({p, s.size()});
        }, py::arg("stream"))
        .def("microprice",           &itch::ItchHandler::microprice)
        .def("order_flow_imbalance", &itch::ItchHandler::order_flow_imbalance)
        .def("stats", &itch::ItchHandler::stats,
             py::return_value_policy::reference_internal);
}
