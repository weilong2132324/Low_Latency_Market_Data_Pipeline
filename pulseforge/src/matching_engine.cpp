/*
 * matching_engine.cpp - the matching loop. See matching_engine.hpp.
 */

#include "matching_engine.hpp"

#include <algorithm>

MatchingEngine::MatchingEngine(EngineListener &listener, SelfMatchPolicy stp,
                               Qty max_order_qty)
    : listener_(listener), stp_(stp), max_order_qty_(max_order_qty) {}

uint16_t MatchingEngine::add_symbol(const Symbol &symbol, const BookConfig &cfg) {
    books_.push_back(std::make_unique<OrderBook>(cfg));
    symbols_.push_back(symbol);
    return static_cast<uint16_t>(books_.size());
}

uint16_t MatchingEngine::find_symbol(const Symbol &symbol) const {
    for (size_t i = 0; i < symbols_.size(); i++) {
        if (symbols_[i] == symbol) {
            return static_cast<uint16_t>(i + 1);
        }
    }
    return 0;
}

void MatchingEngine::submit(uint16_t locate, const OrderRequest &req) {
    if (!valid_locate(locate)) {
        listener_.on_rejected(locate, req, RejectReason::UnknownSymbol);
        return;
    }
    OrderBook &book = *books_[locate - 1];

    /* ---- venue-side validation ---- */
    if (req.qty == 0 || req.qty > max_order_qty_) {
        listener_.on_rejected(locate, req, RejectReason::BadQty);
        return;
    }
    if (!book.valid_price(req.price)) {
        listener_.on_rejected(locate, req, RejectReason::BadPrice);
        return;
    }
    if (book.find(req.id) != nullptr) {
        listener_.on_rejected(locate, req, RejectReason::DuplicateId);
        return;
    }
    listener_.on_accepted(locate, req);

    /* ---- the matching loop ---- */
    const Side contra = opposite(req.side);
    Qty remaining = req.qty;

    while (remaining > 0) {
        const BookOrder *maker = book.front(contra);
        if (maker == nullptr) {
            break; /* nothing left on the other side */
        }
        const bool crosses = req.side == Side::Buy ? maker->price <= req.price
                                                   : maker->price >= req.price;
        if (!crosses) {
            break; /* best contra price is beyond our limit */
        }

        if (req.owner != 0 && maker->owner == req.owner) {
            /* self-match prevention */
            if (stp_ == SelfMatchPolicy::CancelResting) {
                const BookOrder before = *maker; /* copy: remove() invalidates maker */
                book.remove(before.id);
                listener_.on_resting_cancelled(locate, before, before.qty,
                                               CancelReason::SelfMatch);
                continue;
            }
            listener_.on_taker_cancelled(locate, req, remaining, CancelReason::SelfMatch);
            return;
        }

        const Qty q = std::min(remaining, maker->qty);
        Fill f;
        f.match_number    = next_match_++;
        f.price           = maker->price;
        f.qty             = q;
        f.maker_id        = maker->id;
        f.maker_owner     = maker->owner;
        f.maker_user_ref  = maker->user_ref;
        f.maker_side      = maker->side;
        f.maker_remaining = maker->qty - q;
        f.taker_id        = req.id;
        f.taker_owner     = req.owner;
        f.taker_user_ref  = req.user_ref;
        remaining -= q;
        f.taker_remaining = remaining;

        book.reduce_front(contra, q); /* maker pointer is invalid after this */
        listener_.on_fill(locate, f);
    }

    if (remaining == 0) {
        return;
    }
    if (req.tif == TimeInForce::IOC) {
        listener_.on_taker_cancelled(locate, req, remaining, CancelReason::ImmediateOrCancel);
        return;
    }
    /* Day order: the remainder joins the BACK of the queue at its price. */
    OrderBook::AddResult r =
        book.add(req.id, req.side, req.price, remaining, req.owner, req.user_ref);
    if (r == OrderBook::AddResult::Ok) {
        listener_.on_rested(locate, *book.find(req.id));
    } else {
        /* pool exhausted (or a duplicate id slipped through): the order
         * was accepted, so it must be cancelled, not silently dropped */
        listener_.on_taker_cancelled(locate, req, remaining, CancelReason::Supervisory);
    }
}

MatchingEngine::CancelResult MatchingEngine::cancel(uint16_t locate, OrderId id, Qty new_qty,
                                                    uint32_t owner, CancelReason why) {
    if (!valid_locate(locate)) {
        return CancelResult::UnknownSymbol;
    }
    OrderBook &book = *books_[locate - 1];
    const BookOrder *o = book.find(id);
    if (o == nullptr) {
        return CancelResult::UnknownOrder; /* already filled/cancelled: "too late to cancel" */
    }
    if (o->owner != owner) {
        return CancelResult::NotOwner;
    }
    if (new_qty >= o->qty) {
        return CancelResult::NoChange; /* cancels can only reduce */
    }
    const BookOrder before = *o;
    const Qty cancelled = before.qty - new_qty;
    if (new_qty == 0) {
        book.remove(id);
    } else {
        /* A size reduction keeps time priority (it does not move the
         * order to the back of the queue - an increase would). */
        book.reduce(id, cancelled);
    }
    listener_.on_resting_cancelled(locate, before, cancelled, why);
    return CancelResult::Ok;
}
