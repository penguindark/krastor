#include "formatters.h"

#include "config.h"

namespace krastor {

OutputFormatter::OutputFormatter(const Config& cfg)
    : enabled_(cfg.gui.format_output), pal_(cfg.colors.md)
{
}

std::string OutputFormatter::format(const std::string& raw) const
{
    if (!enabled_)
        return raw;
    return md_to_ansi(raw, hl_, pal_);
}

} // namespace krastor
