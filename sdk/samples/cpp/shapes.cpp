// shapes: C++ with classes and virtual functions, a static constructor, new[] and
// delete[], run-time types (typeid, dynamic_cast), exceptions, and <iostream.h>.
#include <windows.h>
#include <iostream.h>
#include <typeinfo.h>

class Shape {
public:
    virtual ~Shape() {}
    virtual double Area() const = 0;
};

class Square : public Shape {
    double Side;
public:
    Square(double side) : Side(side) {}
    double Area() const { return Side * Side; }
};

class Circle : public Shape {
    double Radius;
public:
    Circle(double radius) : Radius(radius) {}
    double Area() const { return 3.14159265358979 * Radius * Radius; }
};

struct Failure {
    const char *What;
    Failure(const char *what) : What(what) {}
};

static double Size(double size)
{
    if (size < 0)
        throw Failure("a negative size");
    return size;
}

// a static object: its constructor runs before main
static struct Start {
    DWORD Ticks;
    Start() { Ticks = GetTickCount(); }
} Started;

int main()
{
    Shape **shapes = new Shape *[2];
    int i;

    shapes[0] = new Square(Size(2));
    shapes[1] = new Circle(Size(1));
    for (i = 0; i < 2; i++)
        cout << typeid(*shapes[i]).name() << ": area " << shapes[i]->Area() << endl;
    if (dynamic_cast<Circle *>(shapes[1]) && !dynamic_cast<Circle *>(shapes[0]))
        cout << "shape 2 is a circle, shape 1 is not" << endl;
    try {
        Size(-1);
        cout << "no exception" << endl;
    } catch (Failure &failure) {
        cout << "caught " << failure.What << endl;
    }
    cout << "started at " << Started.Ticks << " ms" << endl;
    for (i = 0; i < 2; i++)
        delete shapes[i];
    delete [] shapes;
    return 0;
}
