#include "util/DisplayBadge.hpp"

namespace chatterino {

DisplayBadge::DisplayBadge(QString displayName, QString badgeName)
    : displayName_(std::move(displayName))
    , badgeName_(std::move(badgeName))
{
}

QString DisplayBadge::displayName() const
{
    return this->displayName_;
}

QString DisplayBadge::badgeName() const
{
    return this->badgeName_;
}

}  // namespace chatterino
