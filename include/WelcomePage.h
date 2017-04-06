/*
 * nheko Copyright (C) 2017  Konstantinos Sideris <siderisk@auth.gr>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef WELCOMEPAGE_H
#define WELCOMEPAGE_H

#include <QHBoxLayout>
#include <QLabel>
#include <QSpacerItem>
#include <QVBoxLayout>
#include <QWidget>

#include "RaisedButton.h"

class WelcomePage : public QWidget
{
	Q_OBJECT

public:
	explicit WelcomePage(QWidget *parent = 0);
	~WelcomePage();

signals:
	// Notify that the user wants to login in.
	void userLogin();

	// Notify that the user wants to register.
	void userRegister();

private slots:
	void onLoginButtonClicked();
	void onRegisterButtonClicked();

private:
	QVBoxLayout *top_layout_;
	QHBoxLayout *button_layout_;

	QLabel *intro_banner_;
	QLabel *intro_text_;

	QSpacerItem *button_spacer_;

	RaisedButton *register_button_;
	RaisedButton *login_button_;
};

#endif  // WELCOMEPAGE_H