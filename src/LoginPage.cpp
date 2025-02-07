// SPDX-FileCopyrightText: 2017 Konstantinos Sideris <siderisk@auth.gr>
// SPDX-FileCopyrightText: 2021 Nheko Contributors
// SPDX-FileCopyrightText: 2022 Nheko Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDesktopServices>

#include <mtx/identifiers.hpp>
#include <mtx/requests.hpp>
#include <mtx/responses/login.hpp>

#include "Config.h"
#include "Logging.h"
#include "LoginPage.h"
#include "MainWindow.h"
#include "MatrixClient.h"
#include "SSOHandler.h"
#include "UserSettingsPage.h"

Q_DECLARE_METATYPE(LoginPage::LoginMethod)

using namespace mtx::identifiers;

LoginPage::LoginPage(QObject *parent)
  : QObject(parent)
  , inferredServerAddress_()
{
    [[maybe_unused]] static auto ignored =
      qRegisterMetaType<LoginPage::LoginMethod>("LoginPage::LoginMethod");

    connect(this, &LoginPage::versionOkCb, this, &LoginPage::versionOk, Qt::QueuedConnection);
    connect(this, &LoginPage::versionErrorCb, this, &LoginPage::versionError, Qt::QueuedConnection);
    connect(
      this,
      &LoginPage::loginOk,
      this,
      [this](const mtx::responses::Login &res) {
          loggingIn_ = false;
          emit loggingInChanged();

          http::client()->set_user(res.user_id);
          MainWindow::instance()->showChatPage();
      },
      Qt::QueuedConnection);
}
void
LoginPage::showError(const QString &msg)
{
    loggingIn_ = false;
    emit loggingInChanged();

    error_ = msg;
    emit errorOccurred();
}

void
LoginPage::setHomeserver(QString hs)
{
    if (hs != homeserver_) {
        homeserver_      = hs;
        homeserverValid_ = false;
        emit homeserverChanged();
        http::client()->set_server(hs.toStdString());
        checkHomeserverVersion();
    }
}

void
LoginPage::onMatrixIdEntered()
{
    clearErrors();

    homeserverValid_ = false;
    emit homeserverChanged();

    User user;
    try {
        user = parse<User>(mxid_.toStdString());
    } catch (const std::exception &) {
        mxidError_ = tr("You have entered an invalid Matrix ID  e.g @joe:matrix.org");
        emit mxidErrorChanged();
        return;
    }

    if (user.hostname().empty() || user.localpart().empty()) {
        mxidError_ = tr("You have entered an invalid Matrix ID  e.g @joe:matrix.org");
        emit mxidErrorChanged();
        return;
    } else {
        nhlog::net()->debug("hostname: {}", user.hostname());
    }

    if (user.hostname() != inferredServerAddress_.toStdString()) {
        homeserverNeeded_ = false;
        lookingUpHs_      = true;
        emit lookingUpHsChanged();

        http::client()->set_server(user.hostname());
        http::client()->verify_certificates(
          !UserSettings::instance()->disableCertificateValidation());
        homeserver_ = QString::fromStdString(user.hostname());
        emit homeserverChanged();

        http::client()->well_known(
          [this](const mtx::responses::WellKnown &res, mtx::http::RequestErr err) {
              if (err) {
                  if (err->status_code == 404) {
                      nhlog::net()->info("Autodiscovery: No .well-known.");
                      checkHomeserverVersion();
                      return;
                  }

                  if (!err->parse_error.empty()) {
                      emit versionErrorCb(tr("Autodiscovery failed. Received malformed response."));
                      nhlog::net()->error("Autodiscovery failed. Received malformed response.");
                      return;
                  }

                  emit versionErrorCb(tr("Autodiscovery failed. Unknown error when "
                                         "requesting .well-known."));
                  nhlog::net()->error("Autodiscovery failed. Unknown error when "
                                      "requesting .well-known. {} {}",
                                      err->status_code,
                                      err->error_code);
                  return;
              }

              nhlog::net()->info("Autodiscovery: Discovered '" + res.homeserver.base_url + "'");
              http::client()->set_server(res.homeserver.base_url);
              emit homeserverChanged();
              checkHomeserverVersion();
          });
    }
}

void
LoginPage::checkHomeserverVersion()
{
    clearErrors();

    try {
        User user = parse<User>(mxid_.toStdString());
    } catch (const std::exception &) {
        mxidError_ = tr("You have entered an invalid Matrix ID  e.g @joe:matrix.org");
        emit mxidErrorChanged();
        return;
    }

    http::client()->versions([this](const mtx::responses::Versions &, mtx::http::RequestErr err) {
        if (err) {
            if (err->status_code == 404) {
                emit versionErrorCb(tr("The required endpoints were not found. "
                                       "Possibly not a Matrix server."));
                return;
            }

            if (!err->parse_error.empty()) {
                emit versionErrorCb(tr("Received malformed response. Make sure "
                                       "the homeserver domain is valid."));
                return;
            }

            emit versionErrorCb(
              tr("An unknown error occured. Make sure the homeserver domain is valid."));
            return;
        }

        http::client()->get_login(
          [this](mtx::responses::LoginFlows flows, mtx::http::RequestErr err) {
              if (err || flows.flows.empty())
                  emit versionOkCb(true, false);

              bool ssoSupported      = false;
              bool passwordSupported = false;
              for (const auto &flow : flows.flows) {
                  if (flow.type == mtx::user_interactive::auth_types::sso) {
                      ssoSupported = true;
                  } else if (flow.type == mtx::user_interactive::auth_types::password) {
                      passwordSupported = true;
                  }
              }
              emit versionOkCb(passwordSupported, ssoSupported);
          });
    });
}

void
LoginPage::versionError(const QString &error)
{
    showError(error);

    homeserverNeeded_ = true;
    lookingUpHs_      = false;
    homeserverValid_  = false;
    emit lookingUpHsChanged();
    emit versionLookedUp();
}

void
LoginPage::versionOk(bool passwordSupported, bool ssoSupported)
{
    passwordSupported_ = passwordSupported;
    ssoSupported_      = ssoSupported;

    lookingUpHs_     = false;
    homeserverValid_ = true;
    emit homeserverChanged();
    emit lookingUpHsChanged();
    emit versionLookedUp();
}

void
LoginPage::onLoginButtonClicked(LoginMethod loginMethod,
                                QString userid,
                                QString password,
                                QString deviceName)
{
    clearErrors();

    User user;

    try {
        user = parse<User>(userid.toStdString());
    } catch (const std::exception &) {
        mxidError_ = tr("You have entered an invalid Matrix ID  e.g @joe:matrix.org");
        emit mxidErrorChanged();
        return;
    }

    if (loginMethod == LoginMethod::Password) {
        if (password.isEmpty())
            return showError(tr("Empty password"));

        http::client()->login(
          user.localpart(),
          password.toStdString(),
          deviceName.trimmed().isEmpty() ? initialDeviceName_() : deviceName.toStdString(),
          [this](const mtx::responses::Login &res, mtx::http::RequestErr err) {
              if (err) {
                  auto error = err->matrix_error.error;
                  if (error.empty())
                      error = err->parse_error;

                  showError(QString::fromStdString(error));
                  return;
              }

              if (res.well_known) {
                  http::client()->set_server(res.well_known->homeserver.base_url);
                  nhlog::net()->info("Login requested to user server: " +
                                     res.well_known->homeserver.base_url);
              }

              emit loginOk(res);
          });
    } else {
        auto sso = new SSOHandler();
        connect(
          sso, &SSOHandler::ssoSuccess, this, [this, sso, userid, deviceName](std::string token) {
              mtx::requests::Login req{};
              req.token = token;
              req.type  = mtx::user_interactive::auth_types::token;
              req.device_id =
                deviceName.trimmed().isEmpty() ? initialDeviceName_() : deviceName.toStdString();
              http::client()->login(
                req, [this](const mtx::responses::Login &res, mtx::http::RequestErr err) {
                    if (err) {
                        showError(QString::fromStdString(err->matrix_error.error));
                        emit errorOccurred();
                        return;
                    }

                    if (res.well_known) {
                        http::client()->set_server(res.well_known->homeserver.base_url);
                        nhlog::net()->info("Login requested to user server: " +
                                           res.well_known->homeserver.base_url);
                    }

                    emit loginOk(res);
                });
              sso->deleteLater();
          });
        connect(sso, &SSOHandler::ssoFailed, this, [this, sso]() {
            showError(tr("SSO login failed"));
            emit errorOccurred();
            sso->deleteLater();
        });

        QDesktopServices::openUrl(
          QString::fromStdString(http::client()->login_sso_redirect(sso->url())));
    }

    loggingIn_ = true;
    emit loggingInChanged();
}
