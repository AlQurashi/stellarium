/*
 * Stellarium
 * This file Copyright (C) 2008 Matthew Gates
 * Horizon system labels (c) 2015 Georg Zotti
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */

#include "LabelMgr.hpp"
#include "StelObjectMgr.hpp"
#include "StelApp.hpp"
#include "StarMgr.hpp"
#include "StelCore.hpp"
#include "StelLocaleMgr.hpp"
#include "StelModuleMgr.hpp"

#include "StelProjector.hpp"
#include "StelModule.hpp"
#include "StelObject.hpp"
#include "StelObjectType.hpp"
#include "StelUtils.hpp"
#include "VecMath.hpp"
#include "StelPainter.hpp"

#include <QString>
#include <QDebug>
#include <QTimer>

// Base class from which other label types inherit
class StelLabel
{
public:
	StelLabel(const QString& text, const QFont& font, const Vec3f& color);
	virtual ~StelLabel() {;}

	//! draw the label on the sky
	//! @param core the StelCore object
	virtual bool draw(StelCore* core, StelPainter& sPainter) = 0;
	//! update fade for on/off action
	virtual void update(double deltaTime);
	//! Set the duration used for the fade in / fade out of the label.
	void setFadeDuration(float duration);
	//! Set the font color used for the font
	void setFontColor(const Vec3f& color);
	//! Show or hide the label.  It will fade in/out.
	void setFlagShow(bool b);
	//! Get value of flag used to turn on and off the label
	bool getFlagShow(void);
	//! Get value of flag used to turn on and off the label
	void setText(const QString& newText);

	QString labelText;
	QFont labelFont;
	Vec3f labelColor;
	LinearFader labelFader;
	bool autoDelete;
	int id;
	QTimer* timer;
};

//! @class SkyLabel
//! Used to create user labels which are bound to some object on the celestial sphere.
//! The object in question can be any existing StelObject or celestial coordinates.
class SkyLabel : public StelLabel
{
public:
	//! @enum Style determined the way the object to which the label is bound
	//! is indicated. 
	enum Style {
		TextOnly,   //!< Just put the label near the object
		Line        //!< Draw a line from the label text to the object
	};

	//! Constructor of a SkyLabel which is attached to an existing object
	//! @param text the text which will be displayed
	//! @param bindObject a pointer to an existing object to which the label will be attached
	//! @param font a pointer to the font to use for this label
	//! @param color choose a color for the label
	//! @param side which side of the object to draw the label, values N, S, E, W, NE, NW, SE, SW, C (C is centred on the object)
	//! @param distance the distance from the object to draw the label.  If < 0.0, placement is automatic.
	//! @param style determines how the label is drawn
	//! @param enclosureSize determines the size of the enclosure for styles Box and Circle
	// TBD: Apparently styles Box and Circle have been removed?
	SkyLabel(const QString& text, StelObjectP bindObject, const QFont& font, Vec3f color,
			 QString side="NE", double distance=-1.0, SkyLabel::Style style=TextOnly, 
	double enclosureSize=0.0);

	virtual ~SkyLabel();
	// SkyLabel(const QString& text, Vec3d coords, QString side="NE", double distance=-1.0, SkyLabel::Style style=TextOnly, double enclosureSize=-1.0);

	//! Draw the label on the sky
	//! @param core the StelCore object
	//! @param sPainter the StelPainter to use for drawing operations
	virtual bool draw(StelCore* core, StelPainter& sPainter);

	static SkyLabel::Style stringToStyle(const QString& s);
	
private:
	StelObjectP labelObject;
	QString labelSide;
	double labelDistance;
	SkyLabel::Style labelStyle;
	double labelEnclosureSize;
};

//! @class HorizonLabel
//! Used to create user labels which are bound to azimuthal coordinates.
class HorizonLabel : public StelLabel
{
public:
	//! Constructor of a HorizonLabel which is to be displayed on an alt-azimuthal position.
	//! @param text the text for the label
	//! @param az  the azimuth, degrees
	//! @param alt the altitude, degrees
	//! @param font the font to use
	//! @param color the color for the label
	HorizonLabel(const QString& text, const float az, const float alt, const QFont& font, const Vec3f& color);
	virtual ~HorizonLabel();

	//! draw the label on the screen
	//! @param core the StelCore object
	//! @param sPainter the StelPainter to use for drawing operations
	virtual bool draw(StelCore* core, StelPainter& sPainter);
private:
	Vec3d altaz; // the vector to the coordinates
};


//! @class ScreenLabel
//! Used to create user labels which are bound to a fixed point on the screen.
class ScreenLabel : public StelLabel
{
public:
	//! Constructor of a ScreenLabel which is to be displayed at a fixed position on the screen.
	//! @param text the text for the label
	//! @param x the x-position on the screen (pixels from the left side)
	//! @param y the y-position on the screen (pixels from the top side)
	//! @param font the font to use
	//! @param color the color for the label
	ScreenLabel(const QString& text, int x, int y, const QFont& font, const Vec3f& color);
	virtual ~ScreenLabel();

	//! draw the label on the screen
	//! @param core the StelCore object
	//! @param sPainter the StelPainter to use for drawing operations
	virtual bool draw(StelCore* core, StelPainter& sPainter);

private:
	int screenX;
	int screenY;
};

/////////////////////
// StelLabel class //
/////////////////////
StelLabel::StelLabel(const QString& text, const QFont& font, const Vec3f& color)
	: labelText(text),
	  labelFont(font),
	  labelColor(color),
	  autoDelete(false),
	  id(0),
	  timer(Q_NULLPTR)
{
}

void StelLabel::update(double deltaTime)
{
	labelFader.update((int)(deltaTime*1000));
}

void StelLabel::setFadeDuration(float duration)
{
	labelFader.setDuration(duration);
}

void StelLabel::setFontColor(const Vec3f& color)
{
	labelColor = color;
}

void StelLabel::setFlagShow(bool b)
{
	labelFader = b;
}

bool StelLabel::getFlagShow(void)
{
	return labelFader;
}

void StelLabel::setText(const QString& newText)
{
	labelText = newText;
}

SkyLabel::Style SkyLabel::stringToStyle(const QString& s)
{
	if (s=="Line")
		return SkyLabel::Line;
	else
		return SkyLabel::TextOnly;
}

////////////////////
// SkyLabel class //
////////////////////
SkyLabel::SkyLabel(const QString& text, StelObjectP bindObject, const QFont& font,
                   Vec3f color, QString side, double distance, SkyLabel::Style style,
                   double enclosureSize)
	: StelLabel(text, font, color),
	  labelObject(bindObject),
	  labelSide(side),
	  labelDistance(distance),
	  labelStyle(style),
	  labelEnclosureSize(enclosureSize)
{
}

SkyLabel::~SkyLabel()
{
}

bool SkyLabel::draw(StelCore* core, StelPainter& sPainter)
{
	if(labelFader.getInterstate() <= 0.0)
		return false;

	Vec3d objectPos = labelObject->getJ2000EquatorialPos(core);
	Vec3d labelXY;
	sPainter.getProjector()->project(objectPos,labelXY);

	sPainter.setFont(labelFont);
			
	double xOffset(0.);
	double yOffset(0.);
	char hJustify = 'c';
	char vJustify = 'c';

	if (labelSide.toUpper().contains("N"))
	{
		yOffset = 1.0;
		vJustify = 'b'; // bottom justify text
	}
	else if (labelSide.toUpper().contains("S"))
	{
		yOffset = -1.0;
		vJustify = 't'; // top justufy text
	}

	if (labelSide.toUpper().contains("E"))
	{
		xOffset = 1.0;
		hJustify = 'l'; // right justify text
	}
	else if (labelSide.toUpper().contains("W"))
	{
		xOffset = -1.0;
		hJustify = 'r'; // left justify text
	}

	if (labelDistance >= 0.0)
	{
		xOffset *= labelDistance;
		yOffset *= labelDistance;
	}
	else
	{
		float shift = 4.f + labelObject->getAngularSize(core)*M_PI/180.*sPainter.getProjector()->getPixelPerRadAtCenter()/1.8f;
		// use the object size
		xOffset *= shift;
		yOffset *= shift;
	}

	double jxOffset(0.);
	double jyOffset(0.); 
	if (hJustify == 'r')
		jxOffset = sPainter.getFontMetrics().width(labelText);
	else if (hJustify == 'c')
		jxOffset = sPainter.getFontMetrics().width(labelText) / 2.;

	if (vJustify == 't')
		jyOffset = sPainter.getFontMetrics().height();
	else if (vJustify == 'c')
		jyOffset = sPainter.getFontMetrics().height() / 2.;

	sPainter.setColor(labelColor[0], labelColor[1], labelColor[2], labelFader.getInterstate());
	sPainter.drawText(labelXY[0]+xOffset-jxOffset, labelXY[1]+yOffset-jyOffset, labelText, 0, 0, 0, false);

	if (labelStyle == SkyLabel::Line)
	{
		sPainter.setBlending(true);

		// screen coordinates of object
		Vec3d objXY;
		sPainter.getProjector()->project(objectPos,objXY);

		double lineEndX = labelXY[0]+xOffset;
		double lineEndY = labelXY[1]+yOffset;

		if (vJustify == 'b')
			lineEndY -= 5;
		else if (vJustify == 't')
			lineEndY += 5;
				
		if (hJustify == 'l')
			lineEndX -= 5;
		else if (hJustify == 'r')
			lineEndX += 5;
				
		sPainter.setColor(labelColor[0], labelColor[1], labelColor[2], labelFader.getInterstate());
		
		sPainter.drawLine2d(lineEndX,lineEndY,objXY[0], objXY[1]);
	}

	return true;
}

///////////////////////
// HorizonLabel class //
///////////////////////
HorizonLabel::HorizonLabel(const QString& text, const float az, const float alt, const QFont& font, const Vec3f& color)
	: StelLabel(text, font, color)
{
	StelUtils::spheToRect((180.0f-az)*M_PI/180.0, alt*M_PI/180.0, altaz);
}

HorizonLabel::~HorizonLabel()
{
}

bool HorizonLabel::draw(StelCore *core, StelPainter& sPainter)
{
	if (labelFader.getInterstate() <= 0.0)
		return false;

	sPainter.setColor(labelColor[0], labelColor[1], labelColor[2], labelFader.getInterstate());
	sPainter.setFont(labelFont);
	StelProjectorP keepProj=sPainter.getProjector(); // we must reset after painting!
	StelProjectorP altazProjector=core->getProjection(StelCore::FrameAltAz, StelCore::RefractionOff);
	sPainter.setProjector(altazProjector);
	sPainter.drawText(altaz, labelText, 0, 0, 0, false);
	sPainter.setProjector(keepProj);
	return true;
}

///////////////////////
// ScreenLabel class //
///////////////////////
ScreenLabel::ScreenLabel(const QString& text, int x, int y, const QFont& font, const Vec3f& color)
	: StelLabel(text, font, color),
	  screenX(x)
{
	QFontMetrics metrics(font);
	screenY = StelApp::getInstance().getCore()->getProjection2d()->getViewportHeight() - y - metrics.height();
}

ScreenLabel::~ScreenLabel()
{
}

bool ScreenLabel::draw(StelCore*, StelPainter& sPainter)
{
	if (labelFader.getInterstate() <= 0.0)
		return false;

	sPainter.setColor(labelColor[0], labelColor[1], labelColor[2], labelFader.getInterstate());
	sPainter.setFont(labelFont);
	sPainter.drawText(screenX, screenY, labelText, 0, 0, 0, false);
	return true;
}

///////////////////////
// LabelMgr class //
///////////////////////
LabelMgr::LabelMgr() : counter(0)
{
	setObjectName("LabelMgr");
}
 
LabelMgr::~LabelMgr()
{
}

void LabelMgr::init()
{
}

void LabelMgr::draw(StelCore* core)
{
	StelPainter sPainter(core->getProjection(StelCore::FrameJ2000));
	for (auto* l : allLabels)
	{
		l->draw(core, sPainter);
	}
}

void LabelMgr::messageTimeout2()
{
	QObject* obj = QObject::sender();
	for (auto* l : allLabels)
	{
		if (l->timer == obj)
		{
			deleteLabel(l->id);
			return;
		}
	}
}

void LabelMgr::messageTimeout1()
{
	QObject* obj = QObject::sender();
	for (auto* l : allLabels)
	{
		if (l->timer == obj)
		{
			disconnect(l->timer, SIGNAL(timeout()), this, SLOT(messageTimeout1()));
			connect(l->timer, SIGNAL(timeout()), this, SLOT(messageTimeout2()));
			l->setFlagShow(false);
			l->timer->setInterval(l->labelFader.getDuration()*1000);
			l->timer->start();
			return;
		}
	}
}

int LabelMgr::appendLabel(StelLabel* l, int autoDeleteTimeoutMs)
{
	if (autoDeleteTimeoutMs > 0)
	{
		QTimer* timer = new QTimer(this);
		l->timer = timer;
		timer->setSingleShot(true);
		timer->setInterval(autoDeleteTimeoutMs);
		connect(timer, SIGNAL(timeout()), this, SLOT(messageTimeout1()));
		timer->start();
	}

	counter++;
	l->id = counter;
	allLabels[counter] = l;
	return counter;
}

int LabelMgr::labelObject(const QString& text,
                          const QString& objectName,
                          bool visible,
                          float fontSize,
                          const QString& fontColor,
                          const QString& side,
			  double labelDistance,
			  const QString& style,
			  bool autoDelete,
			  int autoDeleteTimeoutMs)
{
	QFont font;
	font.setPixelSize(fontSize);
	StelObjectP obj = GETSTELMODULE(StelObjectMgr)->searchByName(objectName);
	if (!obj)
	{
		qWarning() << "LabelMgr::labelObject object not found: " << objectName;
		return -1;
	}
	
	StelLabel* l = new SkyLabel(text, obj, font, StelUtils::htmlColorToVec3f(fontColor), side, labelDistance, SkyLabel::stringToStyle(style));
	if (l==Q_NULLPTR)
		return -1;

	if (visible)
		l->setFlagShow(true);

	l->autoDelete = autoDelete;

	return appendLabel(l, autoDeleteTimeoutMs);
}

int LabelMgr::labelHorizon(const QString& text,
		float az,
		float alt,
		bool visible,
		float fontSize,
		const QString& fontColor,
		bool autoDelete,
		int autoDeleteTimeoutMs)
{
	QFont font;
	font.setPixelSize(fontSize);
	HorizonLabel* l = new HorizonLabel(text, az, alt, font, StelUtils::htmlColorToVec3f(fontColor));
	if (l==Q_NULLPTR)
		return -1;

	if (visible)
		l->setFlagShow(true);

	l->autoDelete = autoDelete;

	return appendLabel(l, autoDeleteTimeoutMs);
}

int LabelMgr::labelScreen(const QString& text,
                          int x,
                          int y,
                          bool visible,
			  float fontSize,
			  const QString& fontColor,
			  bool autoDelete,
			  int autoDeleteTimeoutMs)
{
	QFont font;
	font.setPixelSize(fontSize);
	ScreenLabel* l = new ScreenLabel(text, x, y, font, StelUtils::htmlColorToVec3f(fontColor));
	if (l==Q_NULLPTR)
		return -1;

	if (visible)
		l->setFlagShow(true);

	l->autoDelete = autoDelete;

	return appendLabel(l, autoDeleteTimeoutMs);
}

bool LabelMgr::getLabelShow(int id)
{
	return allLabels[id]->getFlagShow();
}
	
void LabelMgr::setLabelShow(int id, bool show)
{
	allLabels[id]->setFlagShow(show);
}

void LabelMgr::setLabelText(int id, const QString& newText)
{
	allLabels[id]->setText(newText);
}
	
void LabelMgr::deleteLabel(int id)
{
	if (id<0 || !allLabels.contains(id))
		return;

	if (allLabels[id]->timer != NULL)
		allLabels[id]->timer->deleteLater();

	delete allLabels[id];
	allLabels.remove(id);
}
	
void LabelMgr::update(double deltaTime)
{
	for (auto* l : allLabels) 
		l->update(deltaTime);
}
	
double LabelMgr::getCallOrder(StelModuleActionName actionName) const
{
	if (actionName==StelModule::ActionDraw)
		return StelApp::getInstance().getModuleMgr().getModule("LandscapeMgr")->getCallOrder(actionName)+11;
        return 0;
}

int LabelMgr::deleteAllLabels(void)
{
	int count=0;
	for (auto* l : allLabels)
	{
		delete l;
		count++;
	}
	allLabels.clear();
	return count;
}
