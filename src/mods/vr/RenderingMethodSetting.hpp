#pragma once

#include "Mod.hpp"
#include "MonoRenderingPolicy.hpp"

// Unlike ModCombo, serialized values are IDs, not indices into a branch's labels.
class RenderingMethodSetting final : public IModValue {
public:
    RenderingMethodSetting(std::string name, std::span<const uevr::mono::Choice> options)
        : m_name{std::move(name)}, m_options{options} {}
    int32_t value() const { return m_selection.active(); }
    int32_t requested_value() const { return m_selection.requested(); }
    bool is_available(int32_t id) const { return uevr::mono::available(id, m_options); }
    auto& selection() { return m_selection; }
    const auto& selection() const { return m_selection; }
    void request(int32_t value) { m_selection.request(value); }
    bool draw(std::string_view name) override {
        bool changed = false;
        ImGui::PushID(this);
        if (ImGui::BeginCombo(name.data(), uevr::mono::label(requested_value(), m_options))) {
            for (const auto& option : m_options) {
                if (ImGui::Selectable(option.label, requested_value() == option.id)) {
                    request(option.id);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::Button("Reset to default")) { request(0); changed = true; }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        return changed;
    }
    void draw_value(std::string_view name) override {
        ImGui::Text("%s: %s (%d)", name.data(), uevr::mono::label(requested_value(), m_options), requested_value());
    }
    void config_load(const utility::Config& cfg, bool defaults) override {
        m_selection.initialize(defaults ? 0 : cfg.get<int32_t>(m_name).value_or(requested_value()));
    }
    void config_save(utility::Config& cfg) override { cfg.set<int32_t>(m_name, requested_value()); }
    void set(const std::string& value) override { request(std::stoi(value)); }
    std::string get() const override { return std::to_string(requested_value()); }
    std::string get_config_name() const override { return m_name; }
    std::string_view get_config_name_view() const override { return m_name; }
private:
    const std::string m_name;
    const std::span<const uevr::mono::Choice> m_options;
    uevr::mono::Selection m_selection;
};
