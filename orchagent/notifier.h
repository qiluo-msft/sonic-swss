#include "orch.h"

class Notifier : public ExecutableSelectable {
public:
    Notifier(NotificationConsumer *select, Orch *orch)
        : ExecutableSelectable(select, orch)
    {
    }

    NotificationConsumer *getNotificationConsumer() const
    {
        return static_cast<NotificationConsumer *>(getSelectable());
    }

    void execute()
    {
        m_orch->doTask(*getNotificationConsumer());
    }
};
