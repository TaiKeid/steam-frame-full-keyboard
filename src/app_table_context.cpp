#include "framekeyboard/app.hpp"

namespace framekeyboard {
bool App::table_context_allowed() const {
    return cjk_ && cjk_->engine() == "table" && cjk_->table_context_enabled() &&
           text_output_ == &output_ && !dictation_open_ && learning_allowed() &&
           (host_.context.has_value() || output_.focused_window() != 0);
}
void App::clear_table_context() {
    const bool active = table_context_.active();
    table_context_.clear(DeliveredContext::snapshot(output_));
    table_context_window_ = output_.focused_window();
    if (cjk_ && cjk_->engine() == "table" && cjk_->table_context_enabled()) {
        candidates_.cancel(); // End existing captures, but keep every presented label fixed.
        cjk_->clear_table_context();
        if (active) {
            sync_candidates();
            dirty = true;
        }
    }
}
void App::observe_table_context() {
    if (!table_context_allowed()) {
        if (table_context_.active()) {
            clear_table_context();
        }
        return;
    }
    if (table_context_window_ != output_.focused_window()) {
        clear_table_context();
    }
    const auto before = table_context_.text();
    const auto generation = table_context_.generation();
    table_context_.observe(DeliveredContext::snapshot(output_));
    if (generation != table_context_.generation()) {
        candidates_.cancel();
        cjk_->clear_table_context();
    }
    if (before != table_context_.text()) {
        if (table_context_.text().empty()) {
            candidates_.cancel();
            cjk_->clear_table_context();
            sync_candidates();
            dirty = true;
        } else {
            // Receipt only: the next code lookup uses this context. Never move
            // a visible menu, especially underneath a candidate press, from tick.
            cjk_->set_table_context(table_context_.text());
        }
    }
}
std::optional<App::TableDelivery> App::begin_table_delivery() {
    observe_table_context();
    if (!table_context_allowed()) {
        return {};
    }
    return TableDelivery{table_context_.begin(DeliveredContext::snapshot(output_)),
                         output_.focused_window(), host_.context, cjk_.get()};
}
void App::finish_table_delivery(const std::optional<TableDelivery>& ticket, const std::string& text) {
    if (!ticket || ticket->receipt.generation != table_context_.generation()) {
        return;
    }
    if (!table_context_allowed() || ticket->window != output_.focused_window() ||
        ticket->field != host_.context || ticket->composer != cjk_.get()) {
        clear_table_context(); // Never assign an old-target acknowledgement to a new scope.
        return;
    }
    const auto before = table_context_.text();
    table_context_.admitted(ticket->receipt, DeliveredContext::snapshot(output_), text);
    if (before != table_context_.text()) {
        if (table_context_.text().empty()) {
            candidates_.cancel();
            cjk_->clear_table_context();
            sync_candidates();
            dirty = true;
        } else {
            cjk_->set_table_context(table_context_.text());
        }
    }
}
} // namespace framekeyboard
