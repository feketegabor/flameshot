// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#include "widgets/capture/smartregiondetector.h"
#include "widgets/capture/smartregioninteraction.h"

#include <QTest>

class SmartRegionTest : public QObject
{
    Q_OBJECT

private slots:
    void candidateChainRunsFromLeafToWindow();
    void clickAndDragUseDifferentPaths();
    void physicalWheelDirectionIsStable();
    void preselectionConsumesWheelBeforeToolSizing();
};

void SmartRegionTest::candidateChainRunsFromLeafToWindow()
{
    const QRect window(10, 20, 400, 300);
    const QPoint pointer(80, 90);
    const QRect leaf(70, 80, 40, 30);
    const QRect parent(50, 60, 180, 120);
    const QList<QRect> elements{
        leaf,
        leaf,
        QRect(0, 0, 2, 2),
        QRect(500, 500, 40, 40),
        parent,
        QRect(0, 0, 600, 500),
    };

    const QList<SmartRegionCandidate> chain =
      makeSmartRegionCandidateChain(elements, window, pointer);

    QCOMPARE(chain.size(), 3);
    QCOMPARE(chain.at(0).geometry, leaf);
    QCOMPARE(chain.at(1).geometry, parent);
    QCOMPARE(chain.at(2).geometry, window);
}

void SmartRegionTest::clickAndDragUseDifferentPaths()
{
    const QPoint press(100, 100);
    QVERIFY(!SmartRegionInteraction::startsManualDrag(press, QPoint(102, 101)));
    QVERIFY(SmartRegionInteraction::startsManualDrag(press, QPoint(104, 100)));
}

void SmartRegionTest::physicalWheelDirectionIsStable()
{
    const auto traditionalUp =
      SmartRegionInteraction::wheelDecision(false, true, 120, false);
    const auto traditionalDown =
      SmartRegionInteraction::wheelDecision(false, true, -120, false);
    const auto invertedUp =
      SmartRegionInteraction::wheelDecision(false, true, -120, true);
    const auto invertedDown =
      SmartRegionInteraction::wheelDecision(false, true, 120, true);

    QCOMPARE(traditionalUp.direction, 1);
    QCOMPARE(traditionalDown.direction, -1);
    QCOMPARE(invertedUp.direction, 1);
    QCOMPARE(invertedDown.direction, -1);
}

void SmartRegionTest::preselectionConsumesWheelBeforeToolSizing()
{
    const auto pendingCandidate =
      SmartRegionInteraction::wheelDecision(false, true, 0, false);
    const auto selected =
      SmartRegionInteraction::wheelDecision(true, true, 120, false);
    const auto disabled =
      SmartRegionInteraction::wheelDecision(false, false, 120, false);

    QVERIFY(pendingCandidate.consumed);
    QCOMPARE(pendingCandidate.direction, 0);
    QVERIFY(!selected.consumed);
    QVERIFY(!disabled.consumed);
}

QTEST_GUILESS_MAIN(SmartRegionTest)

#include "smartregiontest.moc"
