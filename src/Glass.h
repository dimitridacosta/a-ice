#pragma once

#include <QPainter>
#include <QImage>
#include <QRect>
#include <QColor>

// Constantes du verre (inset = marge d'ombre autour de la carte).
inline constexpr int kGlassInset = 4;    // marge autour de la carte (région blur)
inline constexpr int kGlassBlur  = 10;   // rayon de flou de l'ombre
inline constexpr int kGlassShadowOffset = 3; // décalage de l'ombre vers le bas
inline constexpr int kGlassShadowAlpha = 90; // intensité de l'ombre (avant étalement)

// Box blur séparable (1 passe H + V). Appelée 2x pour un rendu gaussien.
inline QImage boxBlur(const QImage &src, int radius)
{
    if (radius < 1 || src.isNull()) return src;
    const int w = src.width(), h = src.height();
    QImage tmp(src.size(), QImage::Format_ARGB32_Premultiplied);
    QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
    tmp.fill(Qt::transparent);
    out.fill(Qt::transparent);

    // Passe horizontale.
    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *t = reinterpret_cast<QRgb *>(tmp.scanLine(y));
        for (int x = 0; x < w; ++x) {
            int r = 0, g = 0, b = 0, a = 0;
            for (int k = -radius; k <= radius; ++k) {
                int xx = qBound(0, x + k, w - 1);
                const QRgb c = s[xx];
                r += qRed(c); g += qGreen(c); b += qBlue(c); a += qAlpha(c);
            }
            const int n = 2 * radius + 1;
            t[x] = qRgba(r / n, g / n, b / n, a / n);
        }
    }
    // Passe verticale.
    for (int y = 0; y < h; ++y) {
        QRgb *o = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            int r = 0, g = 0, b = 0, a = 0;
            for (int k = -radius; k <= radius; ++k) {
                int yy = qBound(0, y + k, h - 1);
                const QRgb *t = reinterpret_cast<const QRgb *>(tmp.constScanLine(yy));
                const QRgb c = t[x];
                r += qRed(c); g += qGreen(c); b += qBlue(c); a += qAlpha(c);
            }
            const int n = 2 * radius + 1;
            o[x] = qRgba(r / n, g / n, b / n, a / n);
        }
    }
    return out;
}

// Construit l'image d'une ombre douce pour une carte de taille donnée.
// L'image fait (card + 2*pad) avec la carte centrée, pad = blur*2.
inline QImage makeShadowImage(const QSize &cardSize, int radius)
{
    const int pad = kGlassBlur * 2;
    const QSize sz = cardSize + QSize(2 * pad, 2 * pad);
    if (sz.isEmpty()) return {};
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter ip(&img);
        ip.setRenderHint(QPainter::Antialiasing, true);
        ip.setPen(Qt::NoPen);
        ip.setBrush(Qt::black);
        ip.drawRoundedRect(QRect(QPoint(pad, pad), cardSize), radius, radius);
    }
    img = boxBlur(img, kGlassBlur);
    img = boxBlur(img, kGlassBlur); // 2 passes → rendu gaussien, très diffus
    // Module l'alpha final de l'ombre.
    QPainter tp(&img);
    tp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    tp.fillRect(img.rect(), QColor(0, 0, 0, kGlassShadowAlpha));
    tp.end();
    return img;
}

// Dessine une image d'ombre (centrée sur la carte) via le painter fourni.
// À appeler sur la surface PARENTE (conteneur/fenêtre) pour que les ombres
// puissent déborder et se chevaucher entre cartes (pas clippées par la bulle).
inline void drawShadow(QPainter &p, const QImage &shadow, const QRect &cardRect)
{
    const int pad = kGlassBlur * 2;
    p.drawImage(cardRect.topLeft() - QPoint(pad, pad - kGlassShadowOffset), shadow);
}

// Dessine uniquement la carte verre (sans ombre).
inline void paintCard(QPainter &p, const QRect &r, int radius,
                      const QColor &fill, const QColor &border)
{
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(border, 1));
    p.setBrush(fill);
    p.drawRoundedRect(r, radius, radius);
}