//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  UiTestDriver.h
//
//  Native UI test driver. It is compiled into the frontend, but it only activates when
//  ES_DE_UI_TEST_SCRIPT names a scenario file. The driver injects SDL input events, reads the
//  rendered scene and quits; it has no shell, file-write or network facility.
//

#ifndef ES_APP_UI_TEST_DRIVER_H
#define ES_APP_UI_TEST_DRIVER_H

namespace UiTest
{
    // Reads the scenario named by ES_DE_UI_TEST_SCRIPT. Does nothing when it is unset.
    void initialize();

    // Called once per frame, after the frame has been rendered and before it is presented.
    void frame();
} // namespace UiTest

#endif // ES_APP_UI_TEST_DRIVER_H
