#include <iostream>
#include <string>
#include <sstream>
#include <chrono>
#include "functionAnalyzer.h"

using namespace std;
using FuncType = std::function<std::string()>;

auto epochTime = std::chrono::system_clock::now().time_since_epoch().count();

class VersusComparator {
    public:
        VersusComparator(FuncType f1, FuncType f2) : func1(f1), func2(f2) {}

        bool compare()
        {
            OperationType type1 = identifyOperation(func1);
            OperationType type2 = identifyOperation(func2);

            /*if ( type1 == OperationType::Mathematical || type2 == OperationType::Mathematical)
            {
                //loadMathLibrary();
                return 0;
            }*/

            return func1() == func2();
        }

    private:
        FuncType func1, func2;
        string str1, str2;
        OperationType identifyOperation(FuncType func) 
        {
            // Assuming you can convert or obtain the function's expression as a string
            std::string expression = getExpressionFromFunc(func);
            return FunctionAnalyzer::identifyOperation(expression);
        }
        std::string getExpressionFromFunc(FuncType func)
        {
            std::string operation = func(); // "1+1"
    
            // Extracting operands and operator
            int pos = operation.find("+");
            std::string operand1 = operation.substr(0, pos); // "1"
            std::string operand2 = operation.substr(pos + 1); // "1"
            char op = operation[pos]; // '+'

            // Perform calculation based on the extracted values
            int result = std::stoi(operand1) + std::stoi(operand2); // Convert string to int and add
            
            return std::to_string(result);  // Return the result as a string
            /*
            std::string variableX = func();  // The actual expression

            std::ostringstream oss;
            oss << variableX;  // Insert the string into the stream
            return oss.str();  // Return the string version of the stream
            */
            //return func;
        }
        /*
        std::string getExpressionFromFunc(FuncType func)
        {
            str1 = to_string(func1)
            str2 = to_string(func2)
        }
        */

        void loadMathLibrary()
        {
            // library loading mechanism
        }
};

// Dep.
//int func1()
//{
    // https://stackoverflow.com/questions/10091918/is-it-possible-to-save-a-memory-address-to-a-string
    /*const int *x = new int(5);

    ostringstream get_address;
    get_address << x;
    string address = get_address.str();

    // convert address to base 16
    int hex_address = stoi(address, 0, 16);

    // make a new pointer
    int * new_pointer = ( int * ) hex_address;

    // change value of x
    *new_pointer = 3;

    return;*/

    // https://stackoverflow.com/questions/24319197/c-convert-address-of-memory-to-value
    /*
    std::string variableX = "1+1";  // The actual expression

    std::ostringstream oss;
    oss << variableX;  // Insert the string into the stream
    return oss.str();  // Return the string version of the stream
    */
    //return 1+1;
//}

// Workaround For Now
std::string func1() 
{
    return "1 + 1";  // Return the expression as a string
}

std::string func2() 
{
    return "1 + 1";  // Return the expression as a string
}

int main() {
    VersusComparator vs(func1, func2);
    bool result = vs.compare();
    cout << "The functions are " << (result ? "equal." : "not equal.") << endl;

    return 0;
}
